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

#ifndef ISEKAI_HOST_FALCON_FALCON_CONNECTION_STATE_H_
#define ISEKAI_HOST_FALCON_FALCON_CONNECTION_STATE_H_

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_bitmap.h"
#include "isekai/host/falcon/gating_variable.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/rue/bits.h"

namespace isekai {

// Stores all the per-connection metadata related to the various aspects
// of the FALCON protocol such as sliding window, reliability etc.
struct ConnectionState {
  uint8_t version;
  std::unique_ptr<ConnectionMetadata> connection_metadata;
  std::unique_ptr<CongestionControlMetadata> congestion_control_metadata;

  struct TransmitterTransactionMetadata {
    uint32_t base_request_sequence_number = 0;
    uint32_t next_request_sequence_number = 0;
    uint32_t current_solicitation_sequence_number = 0;
  } tx_transaction_metadata;

  struct ReceiverTransactionMetadata {
    uint32_t base_request_sequence_number = 0;
    uint32_t next_request_sequence_number = 0;
  } rx_transaction_metadata;

  struct TransmitterReliabilityMetadata {
    TransmitterReliabilityWindowMetadata request_window_metadata;
    TransmitterReliabilityWindowMetadata data_window_metadata;

    // Holds the actual RTO expiration time at which the RTO event is
    // triggered..
    absl::Duration rto_expiration_base_time = absl::ZeroDuration();
    absl::Duration GetRtoExpirationTime() { return rto_expiration_base_time; }
    void ClearRtoExpirationTime() {
      rto_expiration_base_time = absl::ZeroDuration();
    }

    explicit TransmitterReliabilityMetadata()
        : request_window_metadata(
              TransmitterReliabilityWindowMetadata(WindowType::kRequest)),
          data_window_metadata(
              TransmitterReliabilityWindowMetadata(WindowType::kData)) {}
  } tx_reliability_metadata;

  struct ReceiverReliabilityMetadata {
    ReceiverReliabilityWindowMetadata request_window_metadata;
    ReceiverReliabilityWindowMetadata data_window_metadata;
    // Reflects the number of packets that are implicitly ACKed. This occurs
    // when the BPSN of the receiver windows are moved ahead prior to sending
    // out an ACK.
    uint32_t implicitly_acked_counter = 0;
    // Mapping between TransactionKey and context of received packets.
    absl::flat_hash_map<TransactionKey, std::unique_ptr<ReceivedPacketContext>>
        received_packet_contexts;
    absl::StatusOr<const ReceivedPacketContext* const> GetReceivedPacketContext(
        const TransactionKey& transaction_key) {
      if (received_packet_contexts.contains(transaction_key)) {
        return received_packet_contexts[transaction_key].get();
      } else {
        return absl::NotFoundError("ReceivedPacketContext not found.");
      }
    }
  } rx_reliability_metadata;

  // Last time this connection sent out a packet. Whenever this variable is
  // updated in the scheduler(s), the following variable (is_op_rate_limited)
  // must also be updated to ensure scheduler eligibility is up-to-date and
  // maintained correctly. If is_op_rate_limited is set to true (1), there must
  // be an event scheduled to mark it false (0) at the appropriate time.
  absl::Duration last_packet_send_time = -absl::InfiniteDuration();
  GatingVariable<int> is_op_rate_limited;

  // ULP-RTO is for received packets to be retransmitted.
  absl::Duration ulp_rto = kDefaultUlpRto;

  // This metadata is used to capture the start time of a cwnd pause.
  absl::Duration cwnd_pause_start_time = absl::ZeroDuration();

  explicit ConnectionState(
      std::unique_ptr<ConnectionMetadata> connection_metadata,
      std::unique_ptr<CongestionControlMetadata> congestion_control_metadata,
      int version = 1)
      : version(version),
        connection_metadata(std::move(connection_metadata)),
        congestion_control_metadata(std::move(congestion_control_metadata)) {}
  virtual ~ConnectionState() {}

  // Holds the metadata related to transactions corresponding to a
  // connection, keyed by the RSN + role of the host (initiator or target).
  absl::flat_hash_map<TransactionKey, std::unique_ptr<TransactionMetadata>>
      transactions;

  absl::StatusOr<TransactionMetadata*> GetTransaction(
      const TransactionKey& transaction_key) const {
    auto it = transactions.find(transaction_key);
    if (it != transactions.end()) {
      return it->second.get();
    }
    return absl::NotFoundError("Transaction not found.");
  }

  // Creates or updates a transaction as Falcon receives ULP ops. It returns the
  // (RSN, PacketType) corresponding to the packet.
  UlpTransactionInfo CreateOrUpdateTransactionFromUlp(
      std::unique_ptr<Packet> packet, int solicited_write_threshold,
      OrderingMode ordering_mode, FalconInterface* falcon);
  // Creates or updates a transaction as Falcon receives a packet from network.
  // It returns the expected request length (as per the original request) and
  // the request/response length of the received Falcon transaction.
  NetworkTransactionInfo CreateOrUpdateTransactionFromNetwork(
      std::unique_ptr<Packet> packet, FalconInterface* falcon);
};

};  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_CONNECTION_STATE_H_
