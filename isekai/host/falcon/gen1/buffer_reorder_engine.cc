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

#include "isekai/host/falcon/gen1/buffer_reorder_engine.h"

#include <cstdint>
#include <utility>

#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "glog/logging.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"

namespace isekai {

ProtocolBufferReorderEngine::ProtocolBufferReorderEngine(
    FalconModelInterface* falcon)
    : falcon_(falcon) {}

absl::Status ProtocolBufferReorderEngine::InitializeConnection(
    uint32_t cid, uint32_t initiator_rsn, uint32_t target_rsn,
    uint32_t initiator_ssn, uint32_t target_ssn, OrderingMode ordering_mode) {
  if (reorder_info_.contains(cid)) {
    return absl::AlreadyExistsError("CID already exists in reorder engine.");
  }
  ConnectionReorderMetadata info;
  info.next_initiator_rsn = initiator_rsn;
  info.next_initiator_ssn = initiator_ssn;
  info.next_target_rsn = target_rsn;
  info.next_target_network_rsn = target_rsn;
  info.next_target_ssn = target_ssn;
  info.ordering_mode = ordering_mode;
  reorder_info_[cid] = info;
  return absl::OkStatus();
}

absl::Status ProtocolBufferReorderEngine::DeleteConnection(uint32_t cid) {
  if (!reorder_info_.contains(cid)) {
    return absl::NotFoundError("CID does not exist in reorder engine.");
  }
  reorder_info_.erase(cid);
  return absl::OkStatus();
}

absl::Status ProtocolBufferReorderEngine::InsertPacket(
    falcon::PacketType packet_type, uint32_t cid, uint32_t rsn, uint32_t ssn) {
  if (!reorder_info_.contains(cid)) {
    return absl::NotFoundError("CID does not exist in reorder engine.");
  }
  auto& info = reorder_info_[cid];

  if (info.ordering_mode == OrderingMode::kUnordered) {
    if (packet_type == falcon::PacketType::kPullRequest ||
        packet_type == falcon::PacketType::kPushSolicitedData ||
        packet_type == falcon::PacketType::kPushUnsolicitedData) {
      CHECK(info.target_rsn_buffer.find(rsn) == info.target_rsn_buffer.end());
      // Remember the RSN and packet_type mapping for possible RNR-NACK.
      info.target_rsn_buffer[rsn] =
          ConnectionReorderMetadata::TransactionMetadata(packet_type);
      // Send to ULP.
      SendToUlpTarget(cid, rsn);
    } else {
      falcon_->ReorderCallback(cid, rsn, packet_type);
    }
    return absl::OkStatus();
  }

  switch (packet_type) {
    // Packets going to ULP as initiator (i.e. completions for sent requests).
    case falcon::PacketType::kPullData:
    case falcon::PacketType::kAck:
    case falcon::PacketType::kNack:
      // If the RSN is equal to next expected RSN, call ReorderCallback directly
      // and further check if any out-of-order packets in the buffer are
      // in-order now.
      if (rsn == info.next_initiator_rsn) {
        ++info.next_initiator_rsn;
        falcon_->ReorderCallback(cid, rsn, packet_type);
        CheckInitiatorRsnBuffer(cid);
      } else {
        info.initiator_rsn_buffer[rsn] = packet_type;
        return absl::OkStatus();
      }
      break;

    // Packets going to ULP as target (i.e. requests from initiator).
    case falcon::PacketType::kPullRequest:
    case falcon::PacketType::kPushSolicitedData:
    case falcon::PacketType::kPushUnsolicitedData:
      // Remember the RSN and packet_type mapping for possible RNR-NACK.
      CHECK(info.target_rsn_buffer.find(rsn) == info.target_rsn_buffer.end());
      info.target_rsn_buffer[rsn] =
          ConnectionReorderMetadata::TransactionMetadata(packet_type);
      if (rsn == info.next_target_rsn) {
        // If this is HoL, send to ULP
        SendToUlpTarget(cid, rsn);
        CheckTargetRsnBuffer(cid);
      } else if (info.in_rnr &&
                 packet_type != falcon::PacketType::kPullRequest) {
        // If not HoL and in RNR state and no PullReq, return
        // FailedPreconditionError so the caller can send RNR NACK.
        return absl::FailedPreconditionError(
            "Non-HoL RSN which is in RNR and not inflight to ULP");
      } else {
        // Do nothing if not HoL and not in RNR state.
        return absl::OkStatus();
      }
      break;
    // Packets that need to be processed by FALCON as initiator (in SSN order).
    case falcon::PacketType::kPushGrant:
      if (ssn == info.next_initiator_ssn) {
        falcon_->ReorderCallback(cid, rsn, packet_type);
        ++info.next_initiator_ssn;
        CheckInitiatorSsnBuffer(cid);
      } else {
        info.initiator_ssn_buffer[ssn] = rsn;
        return absl::OkStatus();
      }
      break;

    // Packets that need to be processed by FALCON as target (in SSN order).
    case falcon::PacketType::kPushRequest:
      if (ssn == info.next_target_ssn) {
        falcon_->ReorderCallback(cid, rsn, packet_type);
        ++info.next_target_ssn;
        CheckTargetSsnBuffer(cid);
      } else {
        info.target_ssn_buffer[ssn] = rsn;
        return absl::OkStatus();
      }
      break;

    case falcon::PacketType::kBack:
    case falcon::PacketType::kEack:
      //
      LOG(FATAL) << "Isekai doesn't use EACK/BACK packet type yet.";
      break;

    // Packets not handled by the reorder engine.
    case falcon::PacketType::kResync:
    case falcon::PacketType::kInvalid:
      // Stop simulation since something has gone wrong at this point.
      LOG(FATAL);
  }
  return absl::OkStatus();
}

absl::Status ProtocolBufferReorderEngine::RetryRnrNackedPacket(uint32_t cid,
                                                               uint32_t rsn) {
  if (!reorder_info_.contains(cid)) {
    return absl::NotFoundError("CID does not exist in reorder engine.");
  }
  auto& info = reorder_info_[cid];
  auto it = info.target_rsn_buffer.find(rsn);
  CHECK(it != info.target_rsn_buffer.end());
  // Duplicate PullRequest should not call retry. Only PushData can.
  CHECK(it->second.packet_type != falcon::PacketType::kPullRequest);

  // If this RSN is not inflight to ULP (not sent to ULP yet),
  if (!IsInflightToUlpTarget(it)) {
    if (info.ordering_mode == OrderingMode::kUnordered) {
      // Unordered connection sends it to ULP.
      SendToUlpTarget(cid, info, it);
    } else {
      // Ordered connection:
      if (rsn == info.next_target_rsn) {
        // If this is HoL, send successive RSNs to ULP. Set the retry time of
        // this RSN to Now so that CheckTargetRsnBuffer will send it to ULP.
        it->second.retry_time = falcon_->get_environment()->ElapsedTime();
        CheckTargetRsnBuffer(cid);
      } else if (info.in_rnr) {
        // If not HoL and in RNR state, return FailedPreconditionError so the
        // caller can send RNR NACK.
        return absl::FailedPreconditionError(
            "Non-HoL RSN which is in RNR and not inflight to ULP");
      }
      // Do nothing if not HoL and not in RNR state.
    }
  }
  return absl::OkStatus();
}

// Determines if the given RSN reflects a HoL request or not. Specifically -
// 1. Unordered connection - then every request is considered HoL.
// 2. Ordered connection - is a HoL request if it matches the expected rsn (at
// the target). This function is only called for incoming push requests, pull
// requests and push unsolicited data packets.
bool ProtocolBufferReorderEngine::IsHeadOfLineNetworkRequest(uint32_t cid,
                                                             uint32_t rsn) {
  // Get a handle on the connection state.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto* const connection_state,
                       state_manager->PerformDirectLookup(cid));
  // If the connection is unordered, then every request is considered as HoL.
  if (connection_state->connection_metadata->ordered_mode ==
      OrderingMode::kUnordered) {
    return true;
  } else {
    if (!reorder_info_.contains(cid)) {
      LOG(FATAL) << "CID does not exist in reorder engine.";
    }
    auto& info = reorder_info_[cid];
    return rsn == info.next_target_network_rsn;
  }
}

void ProtocolBufferReorderEngine::HandleRnrNackFromUlp(
    uint32_t cid, uint32_t rsn, absl::Duration rnr_timeout) {
  CHECK(reorder_info_.contains(cid));
  auto& info = reorder_info_[cid];
  if (info.ordering_mode == OrderingMode::kOrdered) {
    if (rsn < info.next_target_rsn) {
      // Rewind next_target_rsn.
      info.next_target_rsn = rsn;
    }
    // Enter RNR state.
    info.in_rnr = true;
    // Remembers the latest RNR timeout (this does not affect already RNR-NACKed
    // packet's timeout, only for future packets).
    info.rnr_timeout = rnr_timeout;
  }
  // Schedule retry timeout for this packet.
  info.target_rsn_buffer[rsn].retry_time =
      falcon_->get_environment()->ElapsedTime() + rnr_timeout;
  CHECK_OK(falcon_->get_environment()->ScheduleEvent(
      rnr_timeout, [cid, rsn, this]() { HandleRnrTimeout(cid, rsn); }));
}

void ProtocolBufferReorderEngine::HandleAckFromUlp(uint32_t cid, uint32_t rsn) {
  auto& info = reorder_info_[cid];
  info.target_rsn_buffer.erase(rsn);
  // Update the network facing RSN at the target and also exit RNR state.
  if (info.ordering_mode == OrderingMode::kOrdered) {
    ++info.next_target_network_rsn;
    info.in_rnr = false;
  }
}

void ProtocolBufferReorderEngine::CheckInitiatorRsnBuffer(uint32_t cid) {
  auto& info = reorder_info_[cid];
  while (!info.initiator_rsn_buffer.empty()) {
    auto head = info.initiator_rsn_buffer.begin();
    // Check if head of the buffer is the next expected RSN.
    if (info.next_initiator_rsn == head->first) {
      falcon_->ReorderCallback(cid, head->first, head->second);
      info.initiator_rsn_buffer.erase(head);
      ++info.next_initiator_rsn;
    } else {
      break;
    }
  }
}

void ProtocolBufferReorderEngine::CheckInitiatorSsnBuffer(uint32_t cid) {
  auto& info = reorder_info_[cid];
  while (!info.initiator_ssn_buffer.empty()) {
    auto head = info.initiator_ssn_buffer.begin();
    if (info.next_initiator_ssn == head->first) {
      falcon_->ReorderCallback(cid, head->second,
                               falcon::PacketType::kPushGrant);
      info.initiator_ssn_buffer.erase(head);
      ++info.next_initiator_ssn;
    } else {
      break;
    }
  }
}

void ProtocolBufferReorderEngine::CheckTargetRsnBuffer(uint32_t cid) {
  auto& info = reorder_info_[cid];
  auto now = falcon_->get_environment()->ElapsedTime();
  for (auto it = info.target_rsn_buffer.find(info.next_target_rsn);
       it != info.target_rsn_buffer.end(); it++) {
    if (info.next_target_rsn == it->first && it->second.retry_time <= now) {
      // Send successive RSN to ULP.
      SendToUlpTarget(cid, info, it);
    } else {
      break;
    }
  }
}

void ProtocolBufferReorderEngine::CheckTargetSsnBuffer(uint32_t cid) {
  auto& info = reorder_info_[cid];
  while (!info.target_ssn_buffer.empty()) {
    auto head = info.target_ssn_buffer.begin();
    if (info.next_target_ssn == head->first) {
      falcon_->ReorderCallback(cid, head->second,
                               falcon::PacketType::kPushRequest);
      info.target_ssn_buffer.erase(head);
      ++info.next_target_ssn;
    } else {
      break;
    }
  }
}

void ProtocolBufferReorderEngine::HandleRnrTimeout(uint32_t cid, uint32_t rsn) {
  auto& info = reorder_info_[cid];
  auto now = falcon_->get_environment()->ElapsedTime();
  // If this timeout is invalid, return.
  auto it = info.target_rsn_buffer.find(rsn);
  if (it == info.target_rsn_buffer.end()) return;
  if (it->second.retry_time != now) return;

  if (info.ordering_mode == OrderingMode::kUnordered) {
    // Send this RSN to ULP.
    SendToUlpTarget(cid, info, it);
  } else {
    // If this timeout is not for HoL, return;
    if (rsn != info.next_target_rsn) return;
    // Send HoL and successive RSN to ULP.
    CheckTargetRsnBuffer(cid);
  }
}

absl::Duration ProtocolBufferReorderEngine::GetRnrTimeout(uint32_t cid) {
  auto it = reorder_info_.find(cid);
  CHECK(it != reorder_info_.end());
  return it->second.rnr_timeout;
}

void ProtocolBufferReorderEngine::SendToUlpTarget(
    uint32_t cid, ConnectionReorderMetadata& info,
    absl::btree_map<uint32_t,
                    ConnectionReorderMetadata::TransactionMetadata>::iterator
        it) {
  it->second.retry_time = absl::InfiniteDuration();
  falcon_->ReorderCallback(cid, it->first, it->second.packet_type);
  if (info.ordering_mode == OrderingMode::kOrdered) {
    ++info.next_target_rsn;
  }
}
void ProtocolBufferReorderEngine::SendToUlpTarget(uint32_t cid, uint32_t rsn) {
  auto& info = reorder_info_[cid];
  auto it = info.target_rsn_buffer.find(rsn);
  SendToUlpTarget(cid, info, it);
}
bool ProtocolBufferReorderEngine::IsInflightToUlpTarget(
    absl::btree_map<uint32_t,
                    ConnectionReorderMetadata::TransactionMetadata>::iterator
        it) {
  return it->second.retry_time == absl::InfiniteDuration();
}

}  // namespace isekai
