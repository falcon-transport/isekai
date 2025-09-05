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

#ifndef ISEKAI_HOST_FALCON_FALCON_PROTOCOL_ACK_COALESCING_ENGINE_H_
#define ISEKAI_HOST_FALCON_FALCON_PROTOCOL_ACK_COALESCING_ENGINE_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"

namespace isekai {

// Contains all the congestion control metadata that needs to be reflected in an
// ACK.
struct CongestionControlMetadataToReflect {
  uint8_t forward_hops : falcon_rue::kForwardHopsBits;
  absl::Duration timestamp_1 = absl::ZeroDuration();
  absl::Duration timestamp_2 = absl::ZeroDuration();
  uint32_t flow_label : kIpv6FlowLabelNumBits;

  CongestionControlMetadataToReflect() {
    forward_hops = 0;
    flow_label = 0;
  }
};

// The entry that the ACK coalescing engine maintains per AckCoalescingKey. It
// contains coalescing state and thresholds, as well as the
// CongestionControlMetadataToReflect that will be reflected in ACK.
struct AckCoalescingEntry {
  // The latest CongestionControlMetadataToReflect stored from a data/request
  // packet belonging to the AckCoalescingKey.
  struct CongestionControlMetadataToReflect cc_metadata_to_reflect;
  // The threshold for the coalescing counter before sending an ACK out.
  uint32_t coalescing_counter_threshold = 0;
  // The duration threshold for the coalescing timer before sending an ACK out.
  absl::Duration coalescing_timeout_threshold = absl::ZeroDuration();
  // The current coalescing counter.
  uint32_t coalescing_counter = 0;
  // If zero, timer is not running. Otherwise, it represents the latest trigger
  // time for coalescing timer.
  absl::Duration timer_trigger_time = absl::ZeroDuration();
  AckCoalescingEntry() {}
};

template <typename T>
class AckCoalescingEngine : public AckCoalescingEngineInterface {
 public:
  explicit AckCoalescingEngine(FalconModelInterface* falcon) : falcon_(falcon) {
    if (falcon->get_config()->has_eack_trigger_by_hole_in_rx_bitmap()) {
      eack_trigger_by_hole_in_rx_bitmap_ =
          falcon->get_config()->eack_trigger_by_hole_in_rx_bitmap();
    }
  }
  // Creates a new coalescing entry for the AckCoalescingKey. If an entry with
  // the provided key exists, exits with a fatal error. . Here, void* represents
  // a pointer to the template variable T. For Gen1, T is AckCoalescingKey.
  void CreateAckCoalescingEntry(const void* key,
                                const ConnectionMetadata& metadata) override;
  // Used to update ACK triggering criteria on receiving packets, and
  // transmitting an ACK, if the condition met.
  absl::Status GenerateAckOrUpdateCoalescingState(
      std::unique_ptr<AckCoalescingKey> key, bool immediate_ack_req,
      int count_increment) override;
  // Sends out an ACK when the transmission criteria is met.
  void TransmitACK(const AckCoalescingKey& key, bool req_own,
                   bool data_own) override;
  // Transmits a NACK packet triggered either by ULP or Falcon sliding-window
  // layer.
  absl::Status TransmitNACK(const AckCoalescingKey& key, uint32_t original_psn,
                            bool is_request_window,
                            falcon::NackCode falcon_nack_code,
                            const UlpNackMetadata* ulp_nack_metadata) override;
  // Piggybacks ACKs on an outgoing packet (transaction or NACK). The
  // packet_type of the packet must be set before calling this function so it
  // can decide which header (Falcon header or NACK header) to write to.
  absl::Status PiggybackAck(const AckCoalescingKey& key,
                            Packet* packet) override;
  // Updates the stored congestion control metadata (to be reflected in an ACK)
  // for the provided AckCoalescingKey upon receiving a new data/request packet.
  absl::Status UpdateCongestionControlMetadataToReflect(
      const AckCoalescingKey& key, const Packet* packet) override;
  // Generates an AckCoalescingKey from an incoming packet.
  std::unique_ptr<AckCoalescingKey> GenerateAckCoalescingKeyFromIncomingPacket(
      const Packet* packet) override;
  // Generates an AckCoalescingKey from an scid value and an OpaqueCookie
  // reflected back from the ULP.
  std::unique_ptr<AckCoalescingKey> GenerateAckCoalescingKeyFromUlp(
      uint32_t scid, const OpaqueCookie& cookie) override;
  // Returns a const pointer for the AckCoalescingEntry corresponding to the
  // AckCoalescingKey. If the key does not exist, it returns
  // absl::NotFoundError.. Here, void* represents a
  // pointer to the templated T type. For Gen1, T is AckCoalescingKey.
  absl::StatusOr<const AckCoalescingEntry* const>
  GetAckCoalescingEntryForTesting(const void* key);

 protected:
  // Returns the flow label that needs to be used for the current N/ACK.
  virtual uint32_t GetAckFlowLabel(
      const ConnectionState* connection_state,
      const AckCoalescingEntry* ack_coalescing_entry);
  // Returns a non-const pointer to the AckCoalescingEntry. If the key does not
  // exist, it exits with a fatal error.  Here, void* represents a
  // pointer to the templated T type. For Gen1, T is AckCoalescingKey.
  AckCoalescingEntry* GetAckCoalescingEntry(const void* key);
  // Sets up the ACK coalescing timer and its handler when it times off.
  absl::Status SetupAckCoalescingTimer(std::unique_ptr<AckCoalescingKey> key);
  // Handles coalescing timer timing out, leading to ACK transmission.
  void HandleAckCoalescingTimeout(const AckCoalescingKey& key);
  // Creates an ACK or E-ACK.
  std::unique_ptr<Packet> CreateExplicitAckOrEack(
      ConnectionState* connection_state,
      AckCoalescingEntry* ack_coalescing_entry, bool req_own, bool data_own);
  // Fill in the basic ACK fields.
  void FillInBaseAckFields(Packet* ack_packet,
                           const ConnectionState* connection_state,
                           const AckCoalescingEntry* ack_coalescing_entry);
  // Fill in the bitmaps in ACK packets.
  virtual void FillInAckBitmaps(
      Packet* ack_packet, const ConnectionState::ReceiverReliabilityMetadata&
                              rx_reliability_metadata);
  // Populates the ACK with the fields stored from the data packet which will be
  // reflected back with this ACK.
  void PopulateAckWithCongestionControlMetadataToReflect(
      Packet* ack_packet, const AckCoalescingEntry* ack_coalescing_entry);
  // Get the ACK coalescing threshold of a connection.
  uint32_t GetAckCoalescingThreshold(
      const AckCoalescingEntry* ack_coalescing_entry);

  FalconModelInterface* falcon_;
  // EACK triggering condition based on data rx bitmap. True: rx bitmap has
  // holes. False: rx bitmap != 0.
  bool eack_trigger_by_hole_in_rx_bitmap_ = true;
  // For Gen1, T is AckCoalescingKey.
  absl::flat_hash_map<T, std::unique_ptr<struct AckCoalescingEntry>>
      ack_coalescing_entries_;
};

typedef AckCoalescingEngine<AckCoalescingKey> Gen1AckCoalescingEngine;

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_PROTOCOL_ACK_COALESCING_ENGINE_H_
