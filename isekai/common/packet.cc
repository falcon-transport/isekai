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

#include "isekai/common/packet.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/str_join.h"
#include "absl/time/time.h"
#include "isekai/host/falcon/falcon.h"

namespace isekai {
namespace {

std::string FormatBool(bool b) { return b ? "true" : "false"; }

std::string FormatFalconProtoType(falcon::ProtocolType protocol_type) {
  switch (protocol_type) {
    case falcon::ProtocolType::kInvalid:
      return "kInvalid";
    case falcon::ProtocolType::kFalcon:
      return "kFalcon";
    case falcon::ProtocolType::kRdma:
      return "kRdma";
    case falcon::ProtocolType::kGnvme:
      return "kGnvme";
  }
}

std::string FormatFalconPacketType(falcon::PacketType packet_type) {
  switch (packet_type) {
    case falcon::PacketType::kInvalid:
      return "kInvalid";
    case falcon::PacketType::kPullRequest:
      return "kPullRequest";
    case falcon::PacketType::kPushRequest:
      return "kPushRequest";
    case falcon::PacketType::kPushGrant:
      return "kPushGrant";
    case falcon::PacketType::kPullData:
      return "kPullData";
    case falcon::PacketType::kPushSolicitedData:
      return "kPushSolicitedData";
    case falcon::PacketType::kPushUnsolicitedData:
      return "kPushUnsolicitedData";
    case falcon::PacketType::kResync:
      return "kResync";
    case falcon::PacketType::kAck:
      return "kAck";
    case falcon::PacketType::kNack:
      return "kNack";
    case falcon::PacketType::kBack:
      return "kBack";
    case falcon::PacketType::kEack:
      return "kEack";
  }
}

std::string FormatRdmaOpcode(Packet::Rdma::Opcode opcode) {
  switch (opcode) {
    case Packet::Rdma::Opcode::kInvalid:
      return "kInvalid";
    case Packet::Rdma::Opcode::kSendOnly:
      return "kSendOnly";
    case Packet::Rdma::Opcode::kWriteOnly:
      return "kWriteOnly";
    case Packet::Rdma::Opcode::kReadRequest:
      return "kReadRequest";
    case Packet::Rdma::Opcode::kReadResponseOnly:
      return "kReadResponseOnly";
    case Packet::Rdma::Opcode::kAck:
      return "kAck";
  }
}

}  // namespace

Packet& Packet::operator=(const Packet& packet) {
  this->metadata = packet.metadata;
  this->timestamps = packet.timestamps;
  this->ack = packet.ack;
  this->nack = packet.nack;
  this->falcon = packet.falcon;
  this->metadata.scid = packet.metadata.scid;
  this->rdma.opcode = packet.rdma.opcode;
  this->rdma.inline_payload_length = packet.rdma.inline_payload_length;
  this->rdma.request_length = packet.rdma.request_length;
  this->rdma.dest_qp_id = packet.rdma.dest_qp_id;
  this->rdma.rsn = packet.rdma.rsn;
  this->rdma.sgl = std::move(packet.rdma.sgl);
  this->packet_type = packet.packet_type;

  return *this;
}

//
std::string Packet::DebugString() const {
  std::ostringstream stream;
  // clang-format off
  stream
      << "{"
      << "\n  metadata {"
      << "\n    traffic_class: " << static_cast<int>(metadata.traffic_class)
      << "\n    timestamp: " << metadata.timestamp
      << "\n    timing_wheel_timestamp: " << metadata.timing_wheel_timestamp
      << "\n    destination_ip_address: "
      << metadata.destination_ip_address.ToString()
      << "\n    source_bifurcation_id: "
      << static_cast<int>(metadata.source_bifurcation_id)
      << "\n    destination_bifurcation_id: "
      << static_cast<int>(metadata.destination_bifurcation_id)
      << "\n    scid: " << metadata.scid;
  stream
      << "\n  }"
      << "\n  falcon {"
      << "\n    protocol_type: " << FormatFalconProtoType(falcon.protocol_type)
      << "\n    packet_type: " << FormatFalconPacketType(packet_type)
      << "\n    ack_req: " << FormatBool(falcon.ack_req)
      << "\n    dest_cid: " << falcon.dest_cid
      << "\n    rrbpsn: " << falcon.rrbpsn
      << "\n    rdbpsn: " << falcon.rdbpsn
      << "\n    psn: " << falcon.psn
      << "\n    rsn: " << falcon.rsn
      << "\n  }"
      << "\n  rdma {"
      << "\n    opcode: " << FormatRdmaOpcode(rdma.opcode)
      << "\n    inline_payload_length: " << rdma.inline_payload_length
      << "\n    request_length: " << rdma.request_length
      << "\n    sgl: [" << absl::StrJoin(rdma.sgl, ", ") << "]"
      << "\n    dest_qp_id: " << rdma.dest_qp_id
      << "\n    rsn: " << rdma.rsn
      << "\n  }"
      << "\n}"
      << "\n";
  // clang-format on
  return stream.str();
}

}  // namespace isekai
