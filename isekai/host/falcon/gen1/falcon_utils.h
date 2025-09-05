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

#ifndef ISEKAI_HOST_FALCON_FALCON_UTILS_H_
#define ISEKAI_HOST_FALCON_FALCON_UTILS_H_

#include <cstdint>
#include <string_view>

#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_types.h"

namespace isekai {

falcon::NackCode AckSyndromeToNackCode(Packet::Syndrome syndrome);

falcon::PacketType RdmaOpcodeToPacketType(const Packet* packet,
                                          int solicited_write_threshold);

// Helper function which returns true if packet belong to request window
bool BelongsToRequestWindow(falcon::PacketType packet_type);

// Returns the Falcon destination connection id from any packet type.
uint32_t GetFalconPacketConnectionId(const Packet& packet);
// Returns true if the packet type is a Falcon transaction.
bool IsFalconTransaction(falcon::PacketType type);
// Returns the next packet type for a given packet type under solicitation.
falcon::PacketType GetNextFalconPacket(falcon::PacketType type);

// Helper function to calculate Falcon TX buffer credits. txPktdataLen is length
// of transfer on rdma_crt_txpktdata interface, it includes CRTBTH and other
// RDMA headers and any inline data if present. txPmdSglLen is length of SGL.
uint32_t CalculateFalconTxBufferCredits(
    uint32_t txPktdataLen, uint32_t txPmdSglLen,
    uint32_t minimum_buffer_allocation_unit);
// Helper function to calculate Falcon RX buffer credits. This would be either
// rxPktdataLen or reqLen depending on the transaction.
uint32_t CalculateFalconRxBufferCredits(
    uint32_t length, uint32_t minimum_buffer_allocation_unit);

TransactionLocation GetTransactionLocation(falcon::PacketType type,
                                           bool incoming);

// Log a packet event.
void LogPacket(absl::Duration elapsed_time, std::string_view host_id,
               const Packet* packet, uint32_t scid, bool tx);
// Log a timeout event.
void LogTimeout(absl::Duration elapsed_time, std::string_view host_id,
                uint32_t scid, const RetransmissionWorkId& work_id,
                RetransmitReason reason);

std::string TypeToString(falcon::PacketType packet_type);
std::string TypeToString(PacketTypeQueue queue_type);

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_UTILS_H_
