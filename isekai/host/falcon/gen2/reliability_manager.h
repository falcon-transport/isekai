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

#ifndef ISEKAI_HOST_FALCON_GEN2_RELIABILITY_MANAGER_H_
#define ISEKAI_HOST_FALCON_GEN2_RELIABILITY_MANAGER_H_

#include <cstdint>
#include <memory>

#include "absl/container/flat_hash_map.h"
#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/packet_reliability_manager.h"
#include "isekai/host/falcon/gen1/weighted_round_robin_policy.h"

namespace isekai {

class Gen2ReliabilityManager : public ProtocolPacketReliabilityManager {
 public:
  explicit Gen2ReliabilityManager(FalconModelInterface* falcon);
  // Initializes the per-connection state stored in the packet reliability
  // manager.
  void InitializeConnection(uint32_t scid) override;

  // Resets the WRR policy for the given connection, forcing the policy to
  // immediately update flow credits using the flow weights in the connection
  // state.
  void ResetWrrForConnection(uint32_t scid);

 protected:
  // Chooses the flow label for a packet depending on its type.
  virtual uint32_t ChooseOutgoingPacketFlowLabel(
      falcon::PacketType packet_type, uint32_t rsn,
      ConnectionState* connection_state) override;
  // Map from connection ID to the per-connection WRR path selection policy.
  // The per-connection WRR policy outputs the next flow ID to schedule a
  // packet on for that connection. The WRR policy can be set to vanilla
  // round-robin during initialization if needed.
  absl::flat_hash_map<uint32_t, std::unique_ptr<PathSelectionPolicy<uint8_t>>>
      path_selection_policies_;
  // Returns the flow label that a retransmission packet should use.
  uint32_t ChooseRetxFlowLabel(const Packet* packet,
                               const ConnectionState* connection_state);

  struct MultipathConfig {
    // The path selection policy for multipath connections. The default policy
    // is weighted round-robin.
    FalconConfig::Gen2ConfigOptions::MultipathConfig::PathSelectionPolicy
        path_selection_policy = FalconConfig::Gen2ConfigOptions::
            MultipathConfig::OPEN_LOOP_FCWND_WEIGHTED_ROUND_ROBIN;
    // With batched packet scheduling mode, the WRR policy will consume all
    // credits for an eligible entity before going over to the next eligible
    // entity. The value of this configuration only matters with a
    // WEIGHTED_ROUND_ROBIN policy.
    bool batched_packet_scheduling = false;

    // The ACK unrolling delay for single path connections.
    absl::Duration single_path_connection_ack_unrolling_delay =
        absl::ZeroDuration();
    // Whether single path connections should process stale ACKs.
    bool single_path_connection_accept_stale_acks = false;
    // The ACK unrolling delay for multipath connections.
    absl::Duration multipath_connection_ack_unrolling_delay =
        absl::ZeroDuration();
    // Whether multipath connections should process stale ACKs.
    bool multipath_connection_accept_stale_acks = false;
    // The policy to use for choosing flow labels for retransmission packets.
    FalconConfig::Gen2ConfigOptions::MultipathConfig::RetxFlowLabel
        retx_flow_label_policy = FalconConfig::Gen2ConfigOptions::
            MultipathConfig::SAME_FLOW_ID_AS_INITIAL_TX;
  } multipath_config_;

 private:
  // Accumulates the number of packets acked to be used for the congestion
  // control algorithm in a RUE event.
  void AccumulateNumAcked(
      ConnectionState* connection_state,
      const OutstandingPacketContext* packet_context) override;
  void DequeuePacketFromRetxScheduler(
      uint32_t scid, uint32_t rsn, falcon::PacketType type,
      TransactionMetadata* transaction) override;
  // Performs the sliding window related processing for incoming packet.
  absl::Status HandleIncomingPacket(const Packet* packet) override;
  // Decrements outstanding request count when an ACK is received unless NIC is
  // configured otherwise.
  void DecrementOutstandingRequestCount(uint32_t scid,
                                        falcon::PacketType type) override;
  // Decrements outstanding request count when a Pull Response is received.
  void DecrementOutstandingRequestCountOnPullResponse(uint32_t scid);
  // Decrements outstanding retransmission request count when an ACK is
  // received unless NIC is configured otherwise.
  void DecrementOutstandingRetransmittedRequestCount(uint32_t scid,
                                                     falcon::PacketType type,
                                                     bool is_acked) override;
  // Decrement outstanding retransmission request count when a Pull Response is
  // received (and if Pull Request was retransmitted).
  void DecrementOutstandingRetransmittedRequestCountOnPullResponse(
      uint32_t scid);
  // Piggybacks ACK information on an outgoing data/request packet.
  void PiggybackAck(uint32_t scid, Packet* packet) override;
  // Handles a stale ACK where RX window BPSN in ACK < TX window BPSN.
  absl::Status HandleStaleAck(const Packet* ack_packet) override;
  // Records the input packet as outstanding and changes the connection and
  // transaction state accordingly.
  void AddToOutstandingPackets(ConnectionState* const connection_state,
                               TransactionMetadata* const transaction,
                               PacketMetadata* const packet_metadata) override;
  void CreateReceivedPacketContext(
      absl::flat_hash_map<TransactionKey,
                          std::unique_ptr<ReceivedPacketContext>>&
          recv_pkt_ctxs,
      const TransactionKey& key) override;

  // Flag to control if ORC decrement should be on pull response or request ACK.
  bool decrement_orc_on_pull_response_ = false;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN2_RELIABILITY_MANAGER_H_
