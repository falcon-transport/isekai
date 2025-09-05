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

#ifndef ISEKAI_HOST_FALCON_FALCON_PROTOCOL_BUFFER_REORDER_ENGINE_H_
#define ISEKAI_HOST_FALCON_FALCON_PROTOCOL_BUFFER_REORDER_ENGINE_H_

#include <cstdint>

#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"

namespace isekai {

// An RX buffer reorder engine that accepts packets in any RSN/SSN order on a
// per-connection basis, and executes the parent FALCON reorder callback
// function in the correct RSN/SSN order packets. Note, it only takes in the
// packet type (along with RSN, SSN and CID), but not the actual packet.

// Conceptually, it follows these rules:
// (1) Anything going to the ULP is RSN ordered. This translates to,
//   (a) As initiator, the completions need to be RSN ordered to ULP.
//       I.e., PullData and ACK/NACKs corresponding to PushData are RSN ordered.
//   (b) As target, requests needs to be RSN ordered to ULP.
//       I.e., PullRequest and PushSolicited/UnsolicitedData are RSN ordered.
// (2) PushGrants (as initiator) and PushRequests (as target) are processed in
//     RSN order w.r.t each other. This is done by keeping track of SSN on both
//     the initiator and the target.
class ProtocolBufferReorderEngine : public BufferReorderEngine {
 public:
  explicit ProtocolBufferReorderEngine(FalconModelInterface* falcon);

  // Initializes a new connection with the given expected next RSN/SSN as
  // initiator and target.
  absl::Status InitializeConnection(uint32_t cid, uint32_t initiator_rsn,
                                    uint32_t target_rsn, uint32_t initiator_ssn,
                                    uint32_t target_ssn,
                                    OrderingMode ordering_mode) override;
  // Deletes a connection, also removing any pending packets in the buffers.
  absl::Status DeleteConnection(uint32_t cid) override;
  // Inserts a given packet type into the reorder buffer with given cid, rsn and
  // ssn. The ssn is only relevant for PushRequest and PushGrant packet types.
  // It is ignored for all other packet types.
  absl::Status InsertPacket(falcon::PacketType packet_type, uint32_t cid,
                            uint32_t rsn, uint32_t ssn) override;
  // Retries a RNR-NACKed packet upon a duplicate packet. Returns
  // FailedPreconditionError if the conection is ordered and in RNR state and
  // this packet is not HoL.
  absl::Status RetryRnrNackedPacket(uint32_t cid, uint32_t rsn) override;
  // Determines if the given RSN reflects a HoL request or not.
  bool IsHeadOfLineNetworkRequest(uint32_t cid, uint32_t rsn) override;
  // Handles RNR NACK.
  void HandleRnrNackFromUlp(uint32_t cid, uint32_t rsn,
                            absl::Duration rnr_timeout) override;
  // Ack a transaction from ULP at Target.
  void HandleAckFromUlp(uint32_t cid, uint32_t rsn) override;
  // Returns the RNR timeout of a connection.
  absl::Duration GetRnrTimeout(uint32_t cid) override;

 protected:
  FalconModelInterface* const falcon_;

 private:
  // These functions check whether the head of the appropriate buffer is
  // eligible to be sent in order for further processing.
  void CheckInitiatorRsnBuffer(uint32_t cid);
  void CheckInitiatorSsnBuffer(uint32_t cid);
  void CheckTargetRsnBuffer(uint32_t cid);
  void CheckTargetSsnBuffer(uint32_t cid);
  // Handle RNR timeout.
  void HandleRnrTimeout(uint32_t cid, uint32_t rsn);

  // Next expected RSN/SSN along with out-of-order packets for a connection.
  struct ConnectionReorderMetadata {
    // Next RSN to be delivered to ULP as initiator (completion).
    uint32_t next_initiator_rsn;
    // Next RSN to be delivered to ULP as target (requests).
    uint32_t next_target_rsn;
    // Next RSN to be accepted from network as target (requests).
    uint32_t next_target_network_rsn;
    // Next SSN for which data can be sent out as initiator.
    uint32_t next_initiator_ssn;
    // Next SSN for which grant can be sent out as target.
    uint32_t next_target_ssn;
    // The ordering mode of this connection.
    OrderingMode ordering_mode;
    // Ordered connection only: if this connection is in RNR state or not.
    bool in_rnr = false;
    // Remembers the latest RNR timeout.
    absl::Duration rnr_timeout = absl::ZeroDuration();

    // List of out-of-order packets.
    // Packets going to ULP as initiator, rsn -> packet_type.
    absl::btree_map<uint32_t, falcon::PacketType> initiator_rsn_buffer;
    struct TransactionMetadata {
      falcon::PacketType packet_type;
      // Retry time == 0 means ready to be sent to ULP. Retry timeout == inf
      // means not eligible to be sent to ULP (e.g., already sent but no
      // response from ULP yet).
      absl::Duration retry_time = absl::ZeroDuration();
      TransactionMetadata() {}
      TransactionMetadata(falcon::PacketType type) : packet_type(type) {}
    };
    // Packets going to ULP as target, rsn -> packet_type.
    absl::btree_map<uint32_t, TransactionMetadata> target_rsn_buffer;
    // Packets processed by FALCON as initiator in SSN order (only PushGrants).
    // Stores ssn -> rsn mapping.
    absl::btree_map<uint32_t, uint32_t> initiator_ssn_buffer;
    // Packets processed by FALCON as target in SSN order (only PushRequests).
    // Stores ssn -> rsn mapping.
    absl::btree_map<uint32_t, uint32_t> target_ssn_buffer;
  };

  // Send transactions to ULP at target.
  void SendToUlpTarget(
      uint32_t cid, ConnectionReorderMetadata& info,
      absl::btree_map<uint32_t,
                      ConnectionReorderMetadata::TransactionMetadata>::iterator
          it);
  void SendToUlpTarget(uint32_t cid, uint32_t rsn);
  // If a transaction is inflight to ULP at target.
  bool IsInflightToUlpTarget(
      absl::btree_map<uint32_t,
                      ConnectionReorderMetadata::TransactionMetadata>::iterator
          it);

  // Map from connection ID to reorder metadata.
  absl::flat_hash_map<uint32_t, ConnectionReorderMetadata> reorder_info_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_PROTOCOL_BUFFER_REORDER_ENGINE_H_
