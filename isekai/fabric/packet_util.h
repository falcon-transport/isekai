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

#ifndef ISEKAI_FABRIC_PACKET_UTIL_H_
#define ISEKAI_FABRIC_PACKET_UTIL_H_

#include <cstdint>
#include <string>

#include "absl/status/status.h"
#include "isekai/common/net_address.h"
#include "isekai/fabric/constants.h"
#include "isekai/fabric/ingress_packet_tag_m.h"
#include "omnetpp/cmessage.h"

namespace isekai {

// Decapsulates / Encapsulates ethernet signal.
omnetpp::cMessage* DecapsulateEthernetSignal(omnetpp::cMessage* packet);
omnetpp::cMessage* EncapsulateEthernetSignal(omnetpp::cMessage* packet);
// Decapsulates / Encapsulates L2 packet.
void DecapsulateMacHeader(omnetpp::cMessage* packet);
void EncapsulateMacHeader(omnetpp::cMessage* packet);
// Sets the source/destination MAC address of the packet.
void SetPacketSourceMac(omnetpp::cMessage* packet,
                        const MacAddress& mac_address);
void SetPacketDestinationMac(omnetpp::cMessage* packet,
                             const MacAddress& mac_address);
// Gets the source MAC address of the packet.
MacAddress GetPacketSourceMac(omnetpp::cMessage* packet);
// Gets the destination MAC address of the packet.
MacAddress GetPacketDestinationMac(omnetpp::cMessage* packet);
// Gets the source IP address of the packet.
std::string GetPacketSourceIp(omnetpp::cMessage* packet);
// Gets the destination IP address of the packet.
std::string GetPacketDestinationIp(omnetpp::cMessage* packet);
// Gets the ID of the port receiving the packet.
uint32_t GetPacketInputPortId(omnetpp::cMessage* packet);
// Gets the Ip protocol type of the packet.
std::string GetPacketIpProtocol(omnetpp::cMessage* packet);
// Gets the L4 source port of the packet.
uint32_t GetPacketUdpSourcePort(const omnetpp::cMessage* packet);
// Gets the L4 destination port of the packet.
uint32_t GetPacketUdpDestinationPort(const omnetpp::cMessage* packet);
// Generates uint32 hash value for packet.
uint32_t GenerateHashValueForPacket(omnetpp::cMessage* packet);
// Decrements the TTL of the packet. Return error if TTL is smaller than 0
// after decrementing.
absl::Status DecrementPacketTtl(omnetpp::cMessage* packet);
// Gets the TTL of the packet.
int GetPacketTtl(omnetpp::cMessage* packet);
// Determines if the received packet is a control flow message or not.
bool IsFlowControlPacket(const omnetpp::cMessage* packet);
//
int GetPacketPriority(const omnetpp::cMessage* packet);
// Gets the packet occupancy in cells.
uint64_t GetPacketOccupancy(const omnetpp::cMessage* packet);
// Constructs PFC message by given the class_enable and class_time
// information.
omnetpp::cMessage* CreatePfcPacket(int priority, int16_t pause_units);
// Updates the pfc_frame based on the given class_enable and class_time.
void PfcFrameUpdate(omnetpp::cMessage* pfc_frame, int priority,
                    int16_t pause_units);
// Set/Get Ecn marking of the packet.
void PacketSetEcn(omnetpp::cMessage* packet, EcnCode ecn_code);
EcnCode PacketGetEcn(omnetpp::cMessage* packet);
void AddIngressTag(omnetpp::cMessage* packet, uint32_t port_id, int priority,
                   uint64_t minimal_guarantee_occupancy,
                   uint64_t shared_pool_occupancy, uint64_t headroom_occupancy);
void GetAndRemoveIngressTag(omnetpp::cMessage* packet, uint32_t& port_id,
                            int& priority,
                            uint64_t& minimal_guarantee_occupancy,
                            uint64_t& shared_pool_occupancy,
                            uint64_t& headroom_occupancy);
uint32_t GetPacketIngressPort(omnetpp::cMessage* packet);
uint32_t GetPacketIngressPriority(omnetpp::cMessage* packet);
void RemovePhyHeader(omnetpp::cMessage* packet);
uint32_t GetFalconPacketScid(const omnetpp::cMessage* packet);
uint32_t GetFalconPacketDestCid(const omnetpp::cMessage* packet);
bool IsFalconAck(const omnetpp::cMessage* packet);
// Gets the flow label at L3.
uint32_t GetPacketFlowLabel(const omnetpp::cMessage* packet);
// Gets the static port list from the Falcon packet.
absl::StatusOr<uint32_t> GetPacketStaticRoutingPort(
    const omnetpp::cMessage* packet);
// Increments the static port index in the Falcon packet.
void IncrementPacketStaticRoutingIndex(omnetpp::cMessage* packet);
// Sets static port list to a Falcon packet. This function is only used for
// test.
void SetFalconPacketStaticPortListForTesting(omnetpp::cMessage* packet,
                                             std::vector<uint32_t> port_list);
};  // namespace isekai

#endif  // ISEKAI_FABRIC_PACKET_UTIL_H_
