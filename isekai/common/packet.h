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

#ifndef ISEKAI_COMMON_PACKET_H_
#define ISEKAI_COMMON_PACKET_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "absl/time/time.h"
#include "isekai/common/constants.h"
#include "isekai/common/net_address.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_bitmap.h"
#include "isekai/host/falcon/rue/format.h"

namespace isekai {

//
constexpr uint32_t kFalconBaseHeaderSize = 192;
constexpr uint32_t kFalconRequestLength = 16;
constexpr uint32_t kFalconGniMetadataSize = 32;
constexpr uint32_t kFalconVendorDefinedSize = 32;
constexpr uint32_t kFalconResyncCodeSize = 8;
constexpr uint32_t kFalconResyncPacketTypeSize = 4;
constexpr uint32_t kFalconAckSize = 424;
constexpr uint32_t kFalconNackSize = 320;
constexpr uint32_t kFalconEackSize = kFalconAckSize + 40;
constexpr uint32_t kFalconTransactionReservedBits = 16;
constexpr uint32_t kFalconResyncReservedBits = 20;
typedef FalconBitmap<kAckPacketBitmapWidth> FalconAckPacketBitmap;

// Packet is a simplified data-structure for storing packet headers and metadata
struct Packet {
  // Verbs error syndrome, used by the RDMA model to provide NACK reason to
  // FALCON and also passed along in the completion callback to the application.
  enum class Syndrome {
    kAck = 0,
    kRnrNak = 32,
    kPsnSeqErrNak = 96,
    kInvReqNak = 97,
    kRemoteAccessErrNak = 98,
    kRemoteOperationalErrNak = 99,
    kInvRDReqNak = 100
  };
  struct Metadata {
    // Common, 6.4.1
    uint8_t traffic_class : 3;  // Determines traffic CoS.
    // RDMA source queue pair ID, required by FALCON to return the completion
    // for an outgoing transaction.
    uint32_t rdma_src_qp_id;
    // Length of SGL in TX PMD.
    uint32_t sgl_length = 0;
    // On ingress, timestamp consists of a single valid bit, 32 bits of
    // nano-second granularity, and 7 bits of sub nano-second granularity
    uint64_t timestamp : 40;  // IEEE 1588 ingress timestamp.
    // Represents the earliest departure time of the packet, and the Traffic
    // Shaper ensures that the packet is not transmitted until the absolute time
    // is greater than or equal to this timestamp. We store this departure time
    // as absl::Duration since the start of simulation.
    // There will be additional checks in the code to ensure it fits within the
    // hardware bounds (bit lengths) and handles rollovers correctly.
    absl::Duration timing_wheel_timestamp;  // HostInfoTXBase, 6.4.6.
    Ipv6Address destination_ip_address;

    // Bifurcated host ids. These ids are introduced for simulation purpose. In
    // actual scenario, each host will have its own IP address that would be
    // sufficient to identify the host.
    uint8_t source_bifurcation_id : 2;
    uint8_t destination_bifurcation_id : 2;

    // Represents the forward hop count calculated by the packet builder for
    // incoming packets.
    uint8_t forward_hops : falcon_rue::kForwardHopsBits;
    // Represents the source connection ID, and is set by RDMA during initiating
    // a transaction.
    uint32_t scid = 0;
    // The flow label of the packet. Will be used by the packet builder to embed
    // in IPv6 header.
    uint32_t flow_label : 20;
    // RDMA sets this to true if this is the last packet of an Op.
    bool is_last_packet = false;
    // The following metadata serves the static routing function for this
    // packet. It is only used for certain experiments that enable static
    // routing.
    struct StaticRouteMetadata {
      // This vector specifies the routing ports that this packet should
      // traverse sequentially. The number of elements in this vector must be
      // equal to the number of routers encountered.
      std::optional<std::vector<uint32_t>> port_list;
      // This is the current port index in the packet; the router will use this
      // index to locate the corresponding port in the list.
      uint32_t current_port_index = 0;
    } static_route;
    // Forces the packet to wait out this specified delay (if it exists) before
    // being sent out on the wire.
    std::optional<uint32_t> added_delay_ns = std::nullopt;
  } metadata;

  struct TimeStamp {
    // The sent_timestamp is used as T1 and T3, while the received_timestamp is
    // used as T2 and T4.
    // Represents the time (in nanoseconds) in which this packet was sent by
    // packet builder.
    absl::Duration sent_timestamp = absl::ZeroDuration();
    // Represents the time (in nanoseconds) in which this packet was received
    // locally.
    absl::Duration received_timestamp = absl::ZeroDuration();
  } timestamps;

  falcon::PacketType packet_type = falcon::PacketType::kInvalid;

  struct Falcon {
    falcon::ProtocolType protocol_type = falcon::ProtocolType::kInvalid;
    bool ack_req = false;   // Immediate acknowledgement request bit.
    uint32_t dest_cid = 0;  // Destination connection id.
    uint32_t rrbpsn =
        0;  // Receiver request window base packet sequence number.
    uint32_t rdbpsn = 0;  // Receiver data window base packet sequence number.
    uint32_t psn = 0;     // Packet sequence number.
    uint32_t rsn = 0;     // Request sequence number.
    uint32_t ssn = 0;     // Solicitation sequence number.

    // Header extensions for the various FALCON packets.
    // Used by Pull Request, Push Request and Push Grant packets.
    uint16_t request_length = 0;  // Size of request in bytes.
    // Used by the Resync packet.
    struct {
      falcon::ResyncCode code;
      falcon::PacketType packet_type;
    } resync;

    // Total size of the FALCON payload, as seen by the packet builder. It
    // includes the RDMA inline data, RDMA headers and payload added via the
    // SGLs. It does not include the FALCON header sizes.
    uint16_t payload_length = 0;
  } falcon;

  // RDMA header and metadata.
  struct Rdma {
    // Source FALCON connection id. This field is metadata only, and is not
    // present in the RDMA packet header.
    uint32_t scid = 0;
    enum class Opcode {
      kInvalid = 0xFF,
      kSendOnly = 0x04,
      kWriteOnly = 0x0A,
      kReadRequest = 0x0C,
      kReadResponseOnly = 0x10,
      kAck = 0x11,
    } opcode = Opcode::kInvalid;
    //
    // absl::variant.
    // Inline payload length for push requests.
    uint32_t inline_payload_length = 0;
    // A scatter-gather list of data, specified as a list of fragment lengths.
    std::vector<uint32_t> sgl;
    uint32_t dest_qp_id = 0;  // Destination QP id.
    uint32_t rsn = 0;         // Request sequence number.

    // Fields below are used by RDMA/FALCON to set correct request lengths and
    // calculate PCR-26 credits.

    // Total length of CRTBTH and other RDMA headers and any inline data if
    // present. It does not include SGL data or padding which Packet Builder
    // adds. This is the amount of data stored in FALCON Tx buffers. Same as
    // txPktdataLen in PCR-26 document.
    uint32_t data_length = 0;

    // Total length of a requested PushData or PullResponse packet (CRTBTH +
    // other RDMA headers + payload). This is used by FALCON in Push/Pull
    // Requests and Push Grants to communicate to the other side how much data
    // will be transferred. Same as reqLen or txPmd.Generic32Customer.reqLen in
    // PCR-26 document. Request_length is used for Push/Pull requests, and
    // response_length for PullResponse for better readability.
    union {
      uint32_t request_length = 0;
      uint32_t response_length;
    };
  } rdma;

  // ACK header and metadata.
  struct Ack {
    enum AckType {
      kAck = 0,
      kEack = 1,  // E-ACK, introduced for better early retransmission.
    } ack_type = kAck;
    uint32_t dest_cid = 0;  // Destination connection id;
    uint32_t rrbpsn =
        0;  // Receiver request window base packet sequence number.
    uint32_t rdbpsn = 0;  // Receiver data window base packet sequence number.
    FalconAckPacketBitmap receiver_request_bitmap = FalconAckPacketBitmap(
        kAckPacketRequestWindowSize);  // Receiver request sequence number
                                       // bitmap.
    FalconAckPacketBitmap receiver_data_bitmap = FalconAckPacketBitmap(
        kAckPacketDataWindowSize);  // Receiver data sequence number bitmap.
    absl::Duration timestamp_1 =
        absl::ZeroDuration();  // Timestamp from PSP header of request packet.
    absl::Duration timestamp_2 =
        absl::ZeroDuration();  // Timestamp when received by receiving NIC.
    // Forward hops count of request packet
    uint8_t forward_hops : falcon_rue::kForwardHopsBits;
    // Quantized EMA receive buffer occupancy level.
    uint8_t rx_buffer_level : falcon_rue::kRxBufferLevelBits;
    // Congestion control metadata exchanged between peer RUEs.
    uint32_t cc_metadata : falcon_rue::kCcMetadataBits;
    // E-ACK: received-bitmap used in FALCONv2 for better retransmission.
    FalconAckPacketBitmap received_bitmap =
        FalconAckPacketBitmap(kRxDataWindowSize);
    // OWN bits: Out of window notification (request and data).
    bool request_own = false;
    bool data_own = false;
    Ack() : forward_hops(0), rx_buffer_level(0), cc_metadata(0) {}
  } ack;

  // NACK header and metadata.
  struct Nack {
    uint32_t dest_cid = 0;  // Destination connection id;
    uint32_t rrbpsn =
        0;  // Receiver request window base packet sequence number.
    uint32_t rdbpsn = 0;    // Receiver data window base packet sequence number.
    uint32_t nack_psn = 0;  // PSN of the packet that is being NACKed.
    bool request_window = false;  // Indicates the NACKed packet window.
    falcon::NackCode code;
    Syndrome ulp_nack_code;
    absl::Duration rnr_timeout;  // Encodes timeout value for RNR NACK code.
    absl::Duration timestamp_1 =
        absl::ZeroDuration();  // Timestamp from PSP header of request packet.
    absl::Duration timestamp_2 =
        absl::ZeroDuration();  // Timestamp when received by receiving NIC.
    // Forward hops count of request packet
    uint8_t forward_hops : falcon_rue::kForwardHopsBits;
    // Quantized EMA receive buffer occupancy level.
    uint8_t rx_buffer_level : falcon_rue::kRxBufferLevelBits;
    // Congestion control metadata exchanged between peer RUEs.
    //
    // OWN bits to RUE. Also, consider having the below bools as single bits.
    uint32_t cc_metadata : falcon_rue::kCcMetadataBits;
    // OWN bits: Out of window notification (request and data).
    bool request_own = false;
    bool data_own = false;
    Nack() : forward_hops(0), rx_buffer_level(0), cc_metadata(0) {}
  } nack;

  // Returns a string representation of this packet.
  std::string DebugString() const;

  // Reloads the equal operator.
  Packet& operator=(const Packet& packet);
};

}  // namespace isekai

#endif  // ISEKAI_COMMON_PACKET_H_
