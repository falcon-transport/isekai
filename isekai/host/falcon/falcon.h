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

#ifndef ISEKAI_HOST_FALCON_FALCON_H_
#define ISEKAI_HOST_FALCON_FALCON_H_

#include <cstdint>
#include <string>

namespace falcon {

enum class ProtocolType : uint8_t {
  kInvalid = 0,
  kFalcon = 1,
  kRdma = 2,
  kGnvme = 3,
};
std::string ProtocolTypeToString(ProtocolType protocol_type);

enum class PacketType : uint8_t {
  kInvalid = 15,
  kPullRequest = 0,
  kPushRequest = 1,
  kPushGrant = 2,
  kPullData = 3,
  kPushSolicitedData = 4,
  kPushUnsolicitedData = 5,
  kResync = 6,
  kAck = 7,
  kNack = 8,
  kBack = 9,
  kEack = 10,
};
std::string PacketTypeToString(PacketType packet_type);

enum class ResyncCode : uint8_t {
  kTargetUlpCompletionInError = 1,
  kLocalXlrFlow = 2,
  kTransactionTimedOut = 4,
  kRemoteXlrFlow = 5,
  kTargetUlpNonRecoverableError = 6,
  kTargetUlpInvalidCidError = 7,
};
std::string ResyncCodeToString(ResyncCode resync_code);

enum class NackCode : uint8_t {
  kNotANack = 0,  // reserved
  kRxResourceExhaustion = 1,
  kUlpReceiverNotReady = 2,
  kXlrDrop = 4,
  kReserved = 5,
  kUlpCompletionInError = 6,
  kUlpNonRecoverableError = 7,
  kInvalidCidError = 8,
};
std::string NackCodeToString(NackCode nack_code);

// Informs the rate update engine (RUE) of the event type being generated.
// See //isekai/host/falcon/rue/format.h
enum class RueEventType : uint8_t {
  kAck = 0,
  kNack = 1,
  kRetransmit = 2,
};
std::string RueEventTypeToString(RueEventType rue_event_type);

// Informs the RUE of the type of delay measurement to perform.
// See //isekai/host/falcon/rue/format.h
enum class DelaySelect : uint8_t {
  kFull = 0,
  kFabric = 1,
  kForward = 2,
  kReverse = 3,
};
std::string DelaySelectToString(DelaySelect delay_select);

// Describes the direction the congestion window last took.
enum class WindowDirection : uint8_t {
  kDecrease = 0,
  kIncrease = 1,
};
std::string WindowDirectionToString(WindowDirection window_direction);

// Describes the reason a packet is being retransmitted.
enum class RetransmitReason : uint8_t {
  kTimeout = 0,
  kEarly = 1,
};
std::string RetransmitReasonToString(RetransmitReason retransmit_reason);

}  // namespace falcon

#endif  // ISEKAI_HOST_FALCON_FALCON_H_
