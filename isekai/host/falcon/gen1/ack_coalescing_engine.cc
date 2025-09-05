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

#include "isekai/host/falcon/gen1/ack_coalescing_engine.h"

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_bitmap.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/ack_nack_scheduler.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/falcon/gen2/falcon_types.h"

namespace isekai {
namespace {}  // namespace

// Helper function to see if receiver has experienced OOW drops.
bool HasExperiencedOowDrops(
    const ConnectionState::ReceiverReliabilityMetadata* const
        rx_reliability_metadata) {
  return (rx_reliability_metadata->request_window_metadata.has_oow_drop ||
          rx_reliability_metadata->data_window_metadata.has_oow_drop);
}

// Creates a new coalescing entry for the AckCoalescingKey. If an entry with
// the provided key exists, exits with a fatal error. Called when a connection
// is initialized. The coalescing thresholds will be read directly from
// ConnectionState::ConnectionMetadata.
template <typename T>
void AckCoalescingEngine<T>::CreateAckCoalescingEntry(
    const void* key, const ConnectionMetadata& metadata) {
  const T* ack_coalescing_key = static_cast<const T*>(key);
  if (ack_coalescing_entries_.find(*ack_coalescing_key) !=
      ack_coalescing_entries_.end()) {
    LOG(FATAL) << "ACK coalescing entry already exists.";
  }
  ack_coalescing_entries_[*ack_coalescing_key] =
      std::make_unique<AckCoalescingEntry>();
  AckCoalescingEntry* ack_coalescing_entry =
      ack_coalescing_entries_[*ack_coalescing_key].get();

  ack_coalescing_entry->coalescing_counter_threshold =
      metadata.ack_coalescing_threshold;
  ack_coalescing_entry->coalescing_timeout_threshold =
      metadata.ack_coalescing_timeout;
}

template <typename T>
absl::StatusOr<const AckCoalescingEntry* const>
AckCoalescingEngine<T>::GetAckCoalescingEntryForTesting(const void* key) {
  const T* ack_coalescing_key = static_cast<const T*>(key);
  auto it = ack_coalescing_entries_.find(*ack_coalescing_key);
  if (it != ack_coalescing_entries_.end()) {
    return it->second.get();
  }
  return absl::NotFoundError("ACK coalescing entry not found.");
}

template <typename T>
AckCoalescingEntry* AckCoalescingEngine<T>::GetAckCoalescingEntry(
    const void* key) {
  const T* ack_coalescing_key = static_cast<const T*>(key);
  auto it = ack_coalescing_entries_.find(*ack_coalescing_key);
  if (it != ack_coalescing_entries_.end()) {
    return it->second.get();
  }
  LOG(FATAL) << "ACK coalescing entry not found.";
}

// Triggers explicit ACK generation if coalescing count threshold is met or an
// immediate ACK is requested. Otherwise, starts the coalescing timer in case
// it's not running.
template <typename T>
absl::Status AckCoalescingEngine<T>::GenerateAckOrUpdateCoalescingState(
    std::unique_ptr<AckCoalescingKey> key, bool immediate_ack_req,
    int count_increment) {
  AckCoalescingEntry* ack_coalescing_entry = GetAckCoalescingEntry(key.get());
  ack_coalescing_entry->coalescing_counter += count_increment;

  // Get a handle on the connection state.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       state_manager->PerformDirectLookup(key->scid));
  ConnectionState::ReceiverReliabilityMetadata* const rx_reliability_metadata =
      &connection_state->rx_reliability_metadata;

  // Ignore AR-bit in the case of OOW bit being set for either of the windows.
  if (immediate_ack_req &&
      falcon_->get_config()
          ->early_retx()
          .eack_own_metadata()
          .ignore_incoming_ar_bit() &&
      (rx_reliability_metadata->request_window_metadata.has_oow_drop ||
       rx_reliability_metadata->data_window_metadata.has_oow_drop)) {
    immediate_ack_req = false;
  }

  if (immediate_ack_req ||
      ack_coalescing_entry->coalescing_counter >=
          GetAckCoalescingThreshold(ack_coalescing_entry)) {
    if (immediate_ack_req) {
      falcon_->get_stats_manager()->UpdateAcksGeneratedCounterDueToAR(
          key->scid);
    } else {
      falcon_->get_stats_manager()
          ->UpdateAcksGeneratedCounterDueToCoalescingCounter(key->scid);
    }
    TransmitACK(*key, /*req_oow=*/false, /*data_oow=*/false);
  } else if (ack_coalescing_entry->timer_trigger_time == absl::ZeroDuration()) {
    return SetupAckCoalescingTimer(std::move(key));
  }
  return absl::OkStatus();
}

// Sends out explicit ACKs when (a) ACK coalescing threshold
// is met; (b) ACK coalescing timer is triggered or (c) an immediate ACK is
// sought. It is done in the following manner -
// 1. Assign values to the various field in the ACK packet.
// 2. Reset the ACK coalescing timer and counter.
template <typename T>
void AckCoalescingEngine<T>::TransmitACK(const AckCoalescingKey& key,
                                         bool req_own, bool data_own) {
  auto scid = key.scid;
  VLOG(2) << "[" << falcon_->get_host_id() << ": "
          << falcon_->get_environment()->ElapsedTime() << "][" << scid
          << ", -, -]"
          << "ACK packet handed over to traffic shaper.";
  // Get a handle on the connection state.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       state_manager->PerformDirectLookup(scid));
  ConnectionState::ReceiverReliabilityMetadata* const rx_reliability_metadata =
      &connection_state->rx_reliability_metadata;

  AckCoalescingEntry* ack_coalescing_entry = GetAckCoalescingEntry(&key);

  // Reset the ACK coalescing counter and timer.
  ack_coalescing_entry->coalescing_counter = 0;
  ack_coalescing_entry->timer_trigger_time = absl::ZeroDuration();
  rx_reliability_metadata->implicitly_acked_counter = 0;

  // Create the ACK.
  auto ack_packet = CreateExplicitAckOrEack(
      connection_state, ack_coalescing_entry, req_own, data_own);

  // Update network Tx counters before sending.
  falcon_->get_stats_manager()->UpdateNetworkTxCounters(
      ack_packet->packet_type, scid, false, RetransmitReason::kTimeout);
  LogPacket(falcon_->get_environment()->ElapsedTime(), falcon_->get_host_id(),
            ack_packet.get(), scid, true);
  // Enqueues the ACK packet to ACK scheduler.
  CHECK_OK(
      static_cast<ProtocolAckNackScheduler*>(falcon_->get_ack_nack_scheduler())
          ->EnqueuePacket(std::move(ack_packet)));
}

// Transmits NACK packet triggered either by ULP or Falcon sliding-window
// layer.
template <typename T>
absl::Status AckCoalescingEngine<T>::TransmitNACK(
    const AckCoalescingKey& key, uint32_t original_psn, bool is_request_window,
    falcon::NackCode falcon_nack_code,
    const UlpNackMetadata* const ulp_nack_metadata) {
  auto scid = key.scid;
  // Get a handle on the connection state.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       state_manager->PerformDirectLookup(scid));

  AckCoalescingEntry* ack_coalescing_entry = GetAckCoalescingEntry(&key);

  // Set the various NACK related fields.
  auto nack_packet = std::make_unique<Packet>();
  nack_packet->packet_type = falcon::PacketType::kNack;
  // Make sure the scid field in the newly created NACK packet's metadata is
  // properly populated.
  nack_packet->metadata.scid = scid;
  // Set the timing wheel metadata to bypass traffic shaper and destination
  // IP.
  nack_packet->metadata.timing_wheel_timestamp = absl::ZeroDuration();
  CHECK_OK_THEN_ASSIGN(
      nack_packet->metadata.destination_ip_address,
      Ipv6Address::OfString(connection_state->connection_metadata->dst_ip));
  // Set bifurcation ids.
  nack_packet->metadata.source_bifurcation_id =
      connection_state->connection_metadata->source_bifurcation_id;
  nack_packet->metadata.destination_bifurcation_id =
      connection_state->connection_metadata->destination_bifurcation_id;
  nack_packet->nack.dest_cid = connection_state->connection_metadata->dcid;
  CHECK_OK(PiggybackAck(key, nack_packet.get()));
  nack_packet->nack.nack_psn = original_psn;
  nack_packet->nack.request_window = is_request_window;
  nack_packet->nack.code = falcon_nack_code;
  // Set the various ULP NACK related fields.
  if ((falcon_nack_code != falcon::NackCode::kRxResourceExhaustion) &&
      (falcon_nack_code != falcon::NackCode::kReserved)) {
    nack_packet->nack.ulp_nack_code = ulp_nack_metadata->ulp_nack_code;
    if (falcon_nack_code == falcon::NackCode::kUlpReceiverNotReady) {
      nack_packet->nack.rnr_timeout = ulp_nack_metadata->rnr_timeout;
    }
  }

  // Set EACK-OWN related fields and reset OWN flags in window metadata.
  ConnectionState::ReceiverReliabilityMetadata* const rx_reliability_metadata =
      &connection_state->rx_reliability_metadata;
  nack_packet->nack.request_own =
      rx_reliability_metadata->request_window_metadata.has_oow_drop;
  nack_packet->nack.data_own =
      rx_reliability_metadata->data_window_metadata.has_oow_drop;
  rx_reliability_metadata->request_window_metadata.has_oow_drop = false;
  rx_reliability_metadata->data_window_metadata.has_oow_drop = false;

  // Set the various CC related NACK fields.
  // Sets the flow label.
  nack_packet->metadata.flow_label =
      GetAckFlowLabel(connection_state, ack_coalescing_entry);
  nack_packet->nack.timestamp_1 =
      ack_coalescing_entry->cc_metadata_to_reflect.timestamp_1;
  nack_packet->nack.timestamp_2 =
      ack_coalescing_entry->cc_metadata_to_reflect.timestamp_2;
  nack_packet->nack.forward_hops =
      ack_coalescing_entry->cc_metadata_to_reflect.forward_hops;
  nack_packet->nack.rx_buffer_level =
      falcon_->get_resource_manager()->GetNetworkRegionOccupancy();
  nack_packet->nack.cc_metadata =
      connection_state->congestion_control_metadata->cc_metadata;

  // Update network Tx counters before sending.
  falcon_->get_stats_manager()->UpdateNetworkTxCounters(
      nack_packet->packet_type, scid, false, RetransmitReason::kTimeout);
  // Enqueues the NACK packet to ACK scheduler.
  CHECK_OK(
      static_cast<ProtocolAckNackScheduler*>(falcon_->get_ack_nack_scheduler())
          ->EnqueuePacket(std::move(nack_packet)));
  return absl::OkStatus();
}

// Responsible for piggy backing any ACKs on the outgoing packet. It does in
// the following manner -
// 1. Decrements the ACK coalescing counter based on the number of implicitly
// ACKed packets.
// 2. Piggy backs the current values of the receiver base PSNs on the packet.
// 3. Stops the ACK coalescing timer in case the coalescing counter becomes 0.
template <typename T>
absl::Status AckCoalescingEngine<T>::PiggybackAck(const AckCoalescingKey& key,
                                                  Packet* const packet) {
  auto scid = key.scid;
  // Get a handle on the connection state.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       state_manager->PerformDirectLookup(scid));
  ConnectionState::ReceiverReliabilityMetadata* const rx_reliability_metadata =
      &connection_state->rx_reliability_metadata;

  AckCoalescingEntry* ack_coalescing_entry = GetAckCoalescingEntry(&key);

  // Update the ACK coalescing related counters only if no OOW drops.
  if (!HasExperiencedOowDrops(rx_reliability_metadata)) {
    if (ack_coalescing_entry->coalescing_counter >=
        rx_reliability_metadata->implicitly_acked_counter * 2) {
      ack_coalescing_entry->coalescing_counter -=
          rx_reliability_metadata->implicitly_acked_counter * 2;
    } else {
      ack_coalescing_entry->coalescing_counter = 0;
    }
    rx_reliability_metadata->implicitly_acked_counter = 0;
  }

  // Update the various piggy back ACK related fields in the packet.
  if (packet->packet_type == falcon::PacketType::kNack) {
    packet->nack.rrbpsn = rx_reliability_metadata->request_window_metadata
                              .base_packet_sequence_number;
    packet->nack.rdbpsn = rx_reliability_metadata->data_window_metadata
                              .base_packet_sequence_number;
  } else {
    packet->falcon.rrbpsn = rx_reliability_metadata->request_window_metadata
                                .base_packet_sequence_number;
    packet->falcon.rdbpsn = rx_reliability_metadata->data_window_metadata
                                .base_packet_sequence_number;
  }

  // Reset the ACK timer if ACK coalescing counter = 0.
  if (ack_coalescing_entry->coalescing_counter == 0) {
    ack_coalescing_entry->timer_trigger_time = absl::ZeroDuration();
  }
  return absl::OkStatus();
}

template <typename T>
absl::Status AckCoalescingEngine<T>::UpdateCongestionControlMetadataToReflect(
    const AckCoalescingKey& key, const Packet* const packet) {
  AckCoalescingEntry* ack_coalescing_entry = GetAckCoalescingEntry(&key);

  ack_coalescing_entry->cc_metadata_to_reflect.forward_hops =
      packet->metadata.forward_hops;
  ack_coalescing_entry->cc_metadata_to_reflect.timestamp_1 =
      packet->timestamps.sent_timestamp;
  ack_coalescing_entry->cc_metadata_to_reflect.timestamp_2 =
      packet->timestamps.received_timestamp;
  ack_coalescing_entry->cc_metadata_to_reflect.flow_label =
      packet->metadata.flow_label;

  return absl::OkStatus();
}

// Sets up the ACK coalescing timer.
template <typename T>
absl::Status AckCoalescingEngine<T>::SetupAckCoalescingTimer(
    std::unique_ptr<AckCoalescingKey> key) {
  AckCoalescingEntry* ack_coalescing_entry = GetAckCoalescingEntry(key.get());
  const absl::Duration elapsed_time = falcon_->get_environment()->ElapsedTime();
  // Initialize the ACK coalescing timer trigger time (used to keep track of
  // invalid ACK timeouts).
  ack_coalescing_entry->timer_trigger_time = elapsed_time;
  // Schedule the ACK coalescing timer for the future.
  RETURN_IF_ERROR(falcon_->get_environment()->ScheduleEvent(
      ack_coalescing_entry->coalescing_timeout_threshold,
      // Here we need to create a unique_ptr copy of the AckCoalescingKey for
      // the callback because the callback's context will outlive the caller's
      // context.
      [this, key = std::move(key)]() { HandleAckCoalescingTimeout(*key); }));
  return absl::OkStatus();
}

// Handles ACK coalescing timer timing out. ACK sent out if this corresponds
// to a valid timeout. An invalid timeout occurs when an ACK packet is sent
// earlier due to (a) AR bit being set in a received packet or (b) ACK counter
// threshold is met or (c) a piggybacked ACK is sent.
template <typename T>
void AckCoalescingEngine<T>::HandleAckCoalescingTimeout(
    const AckCoalescingKey& key) {
  AckCoalescingEntry* ack_coalescing_entry = GetAckCoalescingEntry(&key);

  // If this corresponds to a valid ACK coalescing timeout, then send the
  // ACK if there are any outstanding ACKs.
  const absl::Duration elapsed_time =
      falcon_->get_environment()->ElapsedTime() -
      ack_coalescing_entry->timer_trigger_time;
  if ((elapsed_time == ack_coalescing_entry->coalescing_timeout_threshold) &&
      ack_coalescing_entry->coalescing_counter > 0) {
    falcon_->get_stats_manager()->UpdateAcksGeneratedCounterDueToTimeout(
        key.scid);
    TransmitACK(key, /*req_own=*/false, /*data_own=*/false);
  }
}

// Creates a ACK or E-ACK.
template <typename T>
std::unique_ptr<Packet> AckCoalescingEngine<T>::CreateExplicitAckOrEack(
    ConnectionState* const connection_state,
    AckCoalescingEntry* const ack_coalescing_entry, bool req_own,
    bool data_own) {
  // Get a handle on the connection state.
  ConnectionState::ReceiverReliabilityMetadata* const rx_reliability_metadata =
      &connection_state->rx_reliability_metadata;

  //
  // function parameters (always false as code exists). This requires clean up
  // that will be done in a follow up CL. Specifically, we should get rid of OWN
  // parameters in functions and also we should make TransmitACK function
  // private.
  req_own = rx_reliability_metadata->request_window_metadata.has_oow_drop;
  data_own = rx_reliability_metadata->data_window_metadata.has_oow_drop;

  // Set the various ACK related fields.
  auto ack_packet = std::make_unique<Packet>();
  FillInBaseAckFields(ack_packet.get(), connection_state, ack_coalescing_entry);

  // If data_rx_bitmap should trigger target to send EACK or not.
  bool rx_bitmap_trigger_eack =
      !rx_reliability_metadata->data_window_metadata.receive_window->Empty();
  if (eack_trigger_by_hole_in_rx_bitmap_)
    rx_bitmap_trigger_eack = rx_reliability_metadata->data_window_metadata
                                 .receive_window->FirstHoleIndex() >= 0;
  // If (1) request ack bitmap != 0, (2) data ack bitmap != 0, (3) data rx
  // bitmap should trigger EACK, or (4) OWN bits, send EACK.
  if (!rx_reliability_metadata->request_window_metadata.ack_window->Empty() ||
      !rx_reliability_metadata->data_window_metadata.ack_window->Empty() ||
      rx_bitmap_trigger_eack || req_own || data_own) {
    ack_packet->ack.ack_type = Packet::Ack::kEack;
    // Set all 3 bitmaps.
    FillInAckBitmaps(ack_packet.get(), *rx_reliability_metadata);
    // Set OWN bits.
    ack_packet->ack.request_own = req_own;
    ack_packet->ack.data_own = data_own;
    // Reset the OWN flags in the window metadata as the ACK packet carries this
    // information.
    rx_reliability_metadata->request_window_metadata.has_oow_drop = false;
    rx_reliability_metadata->data_window_metadata.has_oow_drop = false;
  } else {
    // Send ACK.
    ack_packet->ack.ack_type = Packet::Ack::kAck;
  }
  return ack_packet;
}

template <typename T>
void AckCoalescingEngine<T>::FillInBaseAckFields(
    Packet* ack_packet, const ConnectionState* const connection_state,
    const AckCoalescingEntry* const ack_coalescing_entry) {
  const ConnectionState::ReceiverReliabilityMetadata* const
      rx_reliability_metadata = &connection_state->rx_reliability_metadata;

  // Make sure the scid field in the newly created ACK packet's metadata is
  // properly populated.
  ack_packet->metadata.scid = connection_state->connection_metadata->scid;

  // Sets the IP address of the destination.
  CHECK_OK_THEN_ASSIGN(
      ack_packet->metadata.destination_ip_address,
      Ipv6Address::OfString(connection_state->connection_metadata->dst_ip));

  // Set bifurcation ids.
  ack_packet->metadata.source_bifurcation_id =
      connection_state->connection_metadata->source_bifurcation_id;
  ack_packet->metadata.destination_bifurcation_id =
      connection_state->connection_metadata->destination_bifurcation_id;

  // Sets the traffic shaper metadata to bypass the traffic shaper.
  ack_packet->metadata.timing_wheel_timestamp = absl::ZeroDuration();

  // Sets the generic fields.
  ack_packet->packet_type = falcon::PacketType::kAck;
  ack_packet->ack.dest_cid = connection_state->connection_metadata->dcid;

  // Set the various bitmap related ACK fields.
  ack_packet->ack.rrbpsn = rx_reliability_metadata->request_window_metadata
                               .base_packet_sequence_number;
  ack_packet->ack.rdbpsn =
      rx_reliability_metadata->data_window_metadata.base_packet_sequence_number;

  // Get rx buffer data from resource manager.
  ack_packet->ack.rx_buffer_level =
      falcon_->get_resource_manager()->GetNetworkRegionOccupancy();

  // Get cc_metadata populated by RUE.
  ack_packet->ack.cc_metadata =
      connection_state->congestion_control_metadata->cc_metadata;

  // Reflect back congestion control metadata from original data packet.
  ack_packet->metadata.flow_label =
      GetAckFlowLabel(connection_state, ack_coalescing_entry);
  PopulateAckWithCongestionControlMetadataToReflect(ack_packet,
                                                    ack_coalescing_entry);
}

template <typename T>
void AckCoalescingEngine<T>::FillInAckBitmaps(
    Packet* ack_packet, const ConnectionState::ReceiverReliabilityMetadata&
                            rx_reliability_metadata) {
  ack_packet->ack.receiver_request_bitmap =
      dynamic_cast<FalconBitmap<kRxBitmapWidth>&>(
          *(rx_reliability_metadata.request_window_metadata.ack_window));
  ack_packet->ack.receiver_data_bitmap =
      dynamic_cast<FalconBitmap<kRxBitmapWidth>&>(
          *(rx_reliability_metadata.data_window_metadata.ack_window));
  ack_packet->ack.received_bitmap = dynamic_cast<FalconBitmap<kRxBitmapWidth>&>(
      *(rx_reliability_metadata.data_window_metadata.receive_window));
}

// Populates the ACK with the fields stored from the data packet which will be
// reflected back with this ACK.
template <typename T>
void AckCoalescingEngine<T>::PopulateAckWithCongestionControlMetadataToReflect(
    Packet* ack_packet, const AckCoalescingEntry* const ack_coalescing_entry) {
  ack_packet->ack.timestamp_1 =
      ack_coalescing_entry->cc_metadata_to_reflect.timestamp_1;
  ack_packet->ack.timestamp_2 =
      ack_coalescing_entry->cc_metadata_to_reflect.timestamp_2;
  ack_packet->ack.forward_hops =
      ack_coalescing_entry->cc_metadata_to_reflect.forward_hops;
}

template <typename T>
uint32_t AckCoalescingEngine<T>::GetAckCoalescingThreshold(
    const AckCoalescingEntry* const ack_coalescing_entry) {
  // The threshold should double the configured value, because one corresponds
  // to Falcon ACK (received) and another ULP ACK.
  return ack_coalescing_entry->coalescing_counter_threshold * 2;
}

// Returns the flow label that needs to be used for the current N/ACK. For
// Gen1, we use in the ACK the same flow label as that of the data/request
// packets from the ACK host back to the sender host.
template <typename T>
uint32_t AckCoalescingEngine<T>::GetAckFlowLabel(
    const ConnectionState* const connection_state,
    const AckCoalescingEntry* const ack_coalescing_entry) {
  return connection_state->congestion_control_metadata->flow_label;
}

template <typename T>
std::unique_ptr<AckCoalescingKey>
AckCoalescingEngine<T>::GenerateAckCoalescingKeyFromIncomingPacket(
    const Packet* packet) {
  uint32_t scid = GetFalconPacketConnectionId(*packet);
  return std::make_unique<AckCoalescingKey>(scid);
}

template <typename T>
std::unique_ptr<AckCoalescingKey>
AckCoalescingEngine<T>::GenerateAckCoalescingKeyFromUlp(
    uint32_t scid, const OpaqueCookie& cookie) {
  return std::make_unique<AckCoalescingKey>(scid);
}

// Explicit template instantiations.
template class AckCoalescingEngine<AckCoalescingKey>;
template class AckCoalescingEngine<Gen2AckCoalescingKey>;

}  // namespace isekai
