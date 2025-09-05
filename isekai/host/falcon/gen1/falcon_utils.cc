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

#include "isekai/host/falcon/gen1/falcon_utils.h"

#include <cstdint>
#include <string_view>

#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"

namespace isekai {
namespace {

// Section "Resource count calculation equations" in the Falcon MAS-HAS 2.2.
// All values are in bytes.
constexpr uint32_t kFixedTxContextSize = 64;  // CONST_0 in MAS.
constexpr uint32_t kFixedTxMetadataSize = 8;  // CONST_1 in MAS.
constexpr uint32_t kFixedRxContextSize = 0;   // CONST_2 in MAS.
constexpr uint32_t kFixedRxMetadataSize = 0;  // CONST_3 in MAS.

// Computes max(x - y, 0).
int Subtract(int x, int y) { return x >= y ? x - y : 0; }

// Computes ceil(n / q).
int CeilDiv(int n, int q) { return (n + q - 1) / q; }

}  // namespace

falcon::PacketType RdmaOpcodeToPacketType(const Packet* packet,
                                          const int solicited_write_threshold) {
  switch (packet->rdma.opcode) {
    case Packet::Rdma::Opcode::kReadRequest:
      return falcon::PacketType::kPullRequest;
    case Packet::Rdma::Opcode::kReadResponseOnly:
      return falcon::PacketType::kPullData;
    case Packet::Rdma::Opcode::kWriteOnly:
    case Packet::Rdma::Opcode::kSendOnly:
      if (packet->rdma.request_length >= solicited_write_threshold) {
        return falcon::PacketType::kPushRequest;
      } else {
        return falcon::PacketType::kPushUnsolicitedData;
      }
    case Packet::Rdma::Opcode::kAck:
      LOG(FATAL) << "RDMA does not generate an explicit ACK packet in "
                    "RDMA-Falcon mode.";
    case Packet::Rdma::Opcode::kInvalid:
      LOG(FATAL)
          << "Received a packet from RDMA without its opcode initialized.";
  }
}

// Helper function which returns true if packet belong to request window
bool BelongsToRequestWindow(falcon::PacketType packet_type) {
  if (packet_type == falcon::PacketType::kPullRequest ||
      packet_type == falcon::PacketType::kPushRequest) {
    return true;
  } else {
    return false;
  }
}

// Helper function that converts ULP Syndrome to Falcon NACK Code.
falcon::NackCode AckSyndromeToNackCode(Packet::Syndrome syndrome) {
  switch (syndrome) {
    case Packet::Syndrome::kRnrNak:
      return falcon::NackCode::kUlpReceiverNotReady;
    default:
      //
      break;
  }
  return falcon::NackCode::kNotANack;
}

uint32_t GetFalconPacketConnectionId(const Packet& packet) {
  switch (packet.packet_type) {
    case falcon::PacketType::kAck:
      return packet.ack.dest_cid;
    case falcon::PacketType::kNack:
      return packet.nack.dest_cid;
    default:
      return packet.falcon.dest_cid;
  }
}

bool IsFalconTransaction(falcon::PacketType type) {
  switch (type) {
    case falcon::PacketType::kPullRequest:
    case falcon::PacketType::kPushRequest:
    case falcon::PacketType::kPushGrant:
    case falcon::PacketType::kPullData:
    case falcon::PacketType::kPushSolicitedData:
    case falcon::PacketType::kPushUnsolicitedData:
    case falcon::PacketType::kResync:
      return true;
    case falcon::PacketType::kNack:
    case falcon::PacketType::kAck:
    case falcon::PacketType::kEack:
    case falcon::PacketType::kBack:
      return false;
    case falcon::PacketType::kInvalid:
      LOG(FATAL) << "Invalid packet type received from network.";
  }
}

falcon::PacketType GetNextFalconPacket(falcon::PacketType type) {
  if (type == falcon::PacketType::kPushRequest) {
    return falcon::PacketType::kPushGrant;
  } else if (type == falcon::PacketType::kPushGrant) {
    return falcon::PacketType::kPushSolicitedData;
  }
  LOG(FATAL) << "Next Falcon transaction not defined.";
}

// Helper function to calculate Falcon TX buffer credits.
uint32_t CalculateFalconTxBufferCredits(
    uint32_t txPktDataLen, uint32_t txPmdSglLen,
    uint32_t minimum_buffer_allocation_unit) {
  return CeilDiv(Subtract(txPktDataLen, kFixedTxContextSize),
                 minimum_buffer_allocation_unit) +
         CeilDiv(Subtract(txPmdSglLen, kFixedTxMetadataSize),
                 minimum_buffer_allocation_unit);
}

// Helper function to calculate Falcon RX buffer credits.
uint32_t CalculateFalconRxBufferCredits(
    uint32_t length, uint32_t minimum_buffer_allocation_unit) {
  return CeilDiv(Subtract(length, kFixedRxContextSize),
                 minimum_buffer_allocation_unit) +
         kFixedRxMetadataSize;
}

TransactionLocation GetTransactionLocation(falcon::PacketType type,
                                           bool incoming) {
  switch (type) {
    case falcon::PacketType::kPullRequest:
    case falcon::PacketType::kPushRequest:
    case falcon::PacketType::kPushSolicitedData:
    case falcon::PacketType::kPushUnsolicitedData: {
      if (incoming) {
        return TransactionLocation::kTarget;
      }
      return TransactionLocation::kInitiator;
    }
    case falcon::PacketType::kPullData:
    case falcon::PacketType::kPushGrant: {
      if (incoming) {
        return TransactionLocation::kInitiator;
      }
      return TransactionLocation::kTarget;
    }
    default:
      LOG(FATAL) << "ACK/NACK could be either initiator or target.";
  }
}

void LogPacket(absl::Duration elapsed_time, std::string_view host_id,
               const Packet* packet, uint32_t scid, bool tx) {
  // Only LOG when VLOG level >= 2.
  if (!(VLOG_IS_ON(2))) return;
  std::string tx_rx = tx ? "TX" : "RX";
  if (packet->packet_type == falcon::PacketType::kAck) {
    if (packet->ack.ack_type == Packet::Ack::kEack) {
      VLOG(2) << "[" << host_id << ": " << elapsed_time << "][" << scid
              << ", -, -] " << " [" << tx_rx
              << " EACK] T1=" << packet->ack.timestamp_1
              << " rrbpsn=" << packet->ack.rrbpsn
              << " rdbpsn=" << packet->ack.rdbpsn << " req_bitmap="
              << packet->ack.receiver_request_bitmap.ToString()
              << " data_ack_bitmap="
              << packet->ack.receiver_data_bitmap.ToString()
              << " data_rx_bitmap=" << packet->ack.received_bitmap.ToString();
    } else {
      VLOG(2) << "[" << host_id << ": " << elapsed_time << "][" << scid
              << ", -, -] " << " [" << tx_rx
              << " ACK] T1=" << packet->ack.timestamp_1
              << " rrbpsn=" << packet->ack.rrbpsn
              << " rdbpsn=" << packet->ack.rdbpsn;
    }
  } else if (packet->packet_type == falcon::PacketType::kNack) {
    VLOG(2) << "[" << host_id << ": " << elapsed_time << "][" << scid
            << ", -, -] " << " [RX NACK] T1=" << packet->nack.timestamp_1
            << " nack_psn=" << packet->nack.nack_psn
            << " is_req=" << packet->nack.request_window
            << " rrbpsn=" << packet->nack.rrbpsn
            << " rdbpsn=" << packet->nack.rdbpsn;
  } else {
    VLOG(2) << "[" << host_id << ": " << elapsed_time << "][" << scid << ", "
            << packet->falcon.rsn << ", -] " << " [" << tx_rx
            << " pkt] psn=" << packet->falcon.psn
            << " pkt_type=" << (int)packet->packet_type
            << " rrbpsn=" << packet->falcon.rrbpsn
            << " rdbpsn=" << packet->falcon.rdbpsn;
  }
}

void LogTimeout(absl::Duration elapsed_time, std::string_view host_id,
                uint32_t scid, const RetransmissionWorkId& work_id,
                RetransmitReason reason) {
  std::string timeout_type;
  switch (reason) {
    case RetransmitReason::kTimeout:
      timeout_type = "RTO";
      break;
    case RetransmitReason::kUlpRto:
      timeout_type = "ULP-RTO";
      break;
    case RetransmitReason::kEarlyRack:
      timeout_type = "RACK";
      break;
    case RetransmitReason::kEarlyTlp:
      timeout_type = "Tlp";
      break;
    default:
      LOG(FATAL) << "LogTimeout cannot be called with non-timeout reason";
  }
  VLOG(2) << "[" << host_id << ": " << elapsed_time << "][" << scid << ", "
          << work_id.rsn << ", " << (int)work_id.type << "] " << "["
          << timeout_type << " retx] psn=" << work_id.psn;
}

// Helper function that convers packet type enum to string.
std::string TypeToString(falcon::PacketType packet_type) {
  switch (packet_type) {
    case falcon::PacketType::kInvalid:
      return "invalid";
    case falcon::PacketType::kPullRequest:
      return "pull_request";
    case falcon::PacketType::kPushRequest:
      return "push_request";
    case falcon::PacketType::kPushGrant:
      return "push_grant";
    case falcon::PacketType::kPullData:
      return "pull_Data";
    case falcon::PacketType::kPushSolicitedData:
      return "push_solicited_data";
    case falcon::PacketType::kPushUnsolicitedData:
      return "push_unsolicited_data";
    default:
      return "";
  }
}

// Helper function that convers PacketTypeQueue enum in connection scheduler to
// string.
std::string TypeToString(PacketTypeQueue queue_type) {
  switch (queue_type) {
    case PacketTypeQueue::kPullAndOrderedPushRequest:
      return "pull_and_ordered_push_request";
    case PacketTypeQueue::kUnorderedPushRequest:
      return "unordered_push_request";
    case PacketTypeQueue::kPushData:
      return "push_data";
    case PacketTypeQueue::kPushGrant:
      return "push_grant";
    case PacketTypeQueue::kPullData:
      return "pull_Data";
    default:
      return "";
  }
}

}  // namespace isekai
