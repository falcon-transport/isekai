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

#include "isekai/host/falcon/gen1/packet_reliability_manager.h"

#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/net_address.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_bitmap.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/fixed.h"

namespace isekai {

// Helper function to get the correct TX window based on transaction type.
TransmitterReliabilityWindowMetadata* GetAppropriateTxWindow(
    ConnectionState::TransmitterReliabilityMetadata* metadata,
    falcon::PacketType packet_type) {
  if (packet_type == falcon::PacketType::kPullRequest ||
      packet_type == falcon::PacketType::kPushRequest) {
    return &metadata->request_window_metadata;
  } else {
    return &metadata->data_window_metadata;
  }
}

// Helper function to get the correct TX window. This is used during NACK
// processing as they indicate whether the NACK corresponds to request window or
// data window.
TransmitterReliabilityWindowMetadata* GetAppropriateTxWindow(
    ConnectionState::TransmitterReliabilityMetadata* metadata,
    bool request_window) {
  if (request_window) {
    return &metadata->request_window_metadata;
  } else {
    return &metadata->data_window_metadata;
  }
}

// Helper function to get the correct RX window based on packet type.
ReceiverReliabilityWindowMetadata* GetAppropriateRxWindow(
    ConnectionState::ReceiverReliabilityMetadata* metadata,
    falcon::PacketType packet_type) {
  if (packet_type == falcon::PacketType::kPullRequest ||
      packet_type == falcon::PacketType::kPushRequest) {
    return &metadata->request_window_metadata;
  } else {
    return &metadata->data_window_metadata;
  }
}

ProtocolPacketReliabilityManager::ProtocolPacketReliabilityManager(
    FalconModelInterface* falcon)
    : falcon_(falcon),
      ack_token_bucket_(
          falcon_->get_config()->op_boundary_ar_bit().acks_per_sec(),
          absl::Nanoseconds(falcon_->get_config()
                                ->op_boundary_ar_bit()
                                .ack_refill_interval_ns()),
          falcon_->get_config()->op_boundary_ar_bit().ack_burst_size()),
      random_number_generator_(kRandomSeed) {
  CHECK(falcon->get_config()->has_early_retx());
  if (falcon->get_config()->early_retx().has_ooo_count_threshold()) {
    ooo_count_threshold_ =
        falcon->get_config()->early_retx().ooo_count_threshold();
  }
  if (falcon->get_config()->early_retx().has_enable_ooo_count()) {
    enable_ooo_count_ = falcon->get_config()->early_retx().enable_ooo_count();
  }
  if (falcon->get_config()->early_retx().has_ooo_distance_threshold()) {
    ooo_distance_threshold_ =
        falcon->get_config()->early_retx().ooo_distance_threshold();
  }
  if (falcon->get_config()->early_retx().has_enable_ooo_distance()) {
    enable_ooo_distance_ =
        falcon->get_config()->early_retx().enable_ooo_distance();
  }
  if (falcon->get_config()->early_retx().has_enable_eack_own()) {
    enable_eack_own_ = falcon->get_config()->early_retx().enable_eack_own();
    enable_recency_check_bypass_ = falcon->get_config()
                                       ->early_retx()
                                       .eack_own_metadata()
                                       .enable_recency_check_bypass();
    enable_scanning_exit_criteria_bypass_ =
        falcon->get_config()
            ->early_retx()
            .eack_own_metadata()
            .enable_scanning_exit_criteria_bypass();
    enable_smaller_psn_recency_check_bypass_ =
        falcon->get_config()
            ->early_retx()
            .eack_own_metadata()
            .enable_smaller_psn_recency_check_bypass();
    enable_pause_initial_transmission_on_oow_drops_ =
        falcon->get_config()
            ->early_retx()
            .eack_own_metadata()
            .enable_pause_initial_transmission_on_oow_drops();
    request_window_slack_ = falcon->get_config()
                                ->early_retx()
                                .eack_own_metadata()
                                .request_window_slack();
    data_window_slack_ = falcon->get_config()
                             ->early_retx()
                             .eack_own_metadata()
                             .data_window_slack();
  }
  if (falcon->get_config()->early_retx().has_early_retx_threshold()) {
    early_retx_threshold_ =
        falcon->get_config()->early_retx().early_retx_threshold();
  }

  if (falcon_->get_config()->enable_ack_request_bit()) {
    // (1 + b) / 2 = avg = 1/AR
    double average_packets_per_AR =
        100.0 / falcon_->get_config()->ack_request_percent();
    int8_t b = static_cast<uint8_t>(2 * average_packets_per_AR - 1);
    ar_bit_set_generator_ = std::uniform_int_distribution<int32_t>(1, b);
    // Many unit tests assume the first packet will not generate ack.
    next_ar_ =
        static_cast<uint8_t>(ar_bit_set_generator_(random_number_generator_));
  }
}

// Initializes the per-connection state stored in the packet reliability
// manager. No per-connection state is stored in Gen1 packet reliability
// manager.
void ProtocolPacketReliabilityManager::InitializeConnection(uint32_t scid) {}

// Responsible for transmitting the packet picked by the connection scheduler.
// It does the following -
// 1. For first-time transmission, (a) it updates the PSN of the packet based on
// NSNs. Otherwise, (b) uses the PSNs already assigned for retransmissions and
// indicate to the RUE that a retransmission is occurring.
// 2. Initializes the headers of the outgoing packet.
// 3. Setup future retransmission to perform loss recovery in case the target
// does not ACK this packet in the future.
// 4. Piggbacks any outstanding ACKs corresponding to the connection.
// 5. Creates a received packet context corresponding to packets that expect a
// response.
// 6. Send packet out through the traffic shaper.
absl::Status ProtocolPacketReliabilityManager::TransmitPacket(
    uint32_t scid, uint32_t rsn, falcon::PacketType packet_type) {
  // Get a handle on the connection and transaction state along with the packet.
  ConnectionStateManager* state_manager = falcon_->get_state_manager();
  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(scid));
  ASSIGN_OR_RETURN(TransactionMetadata* const transaction,
                   connection_state->GetTransaction(
                       TransactionKey(rsn, GetTransactionLocation(
                                               /*type=*/packet_type,
                                               /*incoming=*/false))));
  ASSIGN_OR_RETURN(PacketMetadata* const packet_metadata,
                   transaction->GetPacketMetadata(packet_type));

  ++packet_metadata->transmit_attempts;
  Packet* const packet = packet_metadata->active_packet.get();
  bool is_retransmission = false;
  // 1a. Assign PSN/SSN to the outgoing packet if it's not a retransmission.
  if (packet_metadata->transmit_attempts == 1) {
    CHECK_OK(AssignPsnAndSsn(connection_state, packet_metadata, packet));
  } else {
    // 1b. Send information to RUE about retransmissions.
    is_retransmission = true;
    if (packet_metadata->retransmission_reason == RetransmitReason::kTimeout) {
      falcon_->get_rate_update_engine()->PacketTimeoutRetransmitted(
          scid, packet, packet_metadata->timeout_retransmission_attempts);
    } else if (packet_metadata->retransmission_reason ==
                   RetransmitReason::kEarlyOooDis ||
               packet_metadata->retransmission_reason ==
                   RetransmitReason::kEarlyRack ||
               packet_metadata->retransmission_reason ==
                   RetransmitReason::kEarlyNack ||
               packet_metadata->retransmission_reason ==
                   RetransmitReason::kEarlyTlp) {
      falcon_->get_rate_update_engine()->PacketEarlyRetransmitted(
          scid, packet, packet_metadata->timeout_retransmission_attempts);
    }
  }
  // 2. Set the various headers of the outgoing packet.
  SetOutgoingPacketHeader(connection_state, rsn, packet_type, packet);
  // 3. Setup retransmission for this outgoing packet.
  CHECK_OK(SetupRetransmission(connection_state, transaction, packet_metadata));
  // 4. Piggy back any outstanding ACKs corresponding to this connection.
  PiggybackAck(scid, packet);
  // 5. Create or update the receiver context of the expected response.
  CreateOrUpdateReceiverPacketContext(connection_state, packet,
                                      PacketDirection::kOutgoing);
  // Increments the outgoing request count (if applicable).
  if (is_retransmission) {
    IncrementOutstandingRetransmittedRequestCount(connection_state,
                                                  packet_type);
  } else {
    IncrementOutstandingRequestCount(connection_state, packet_type);
    // Update the transaction state.
    transaction->UpdateState(packet_type, TransactionStage::NtwkTx, falcon_);
  }
  // Maintain a copy of the packet for future retransmissions.
  packet_metadata->retransmission_packet_copy = *packet_metadata->active_packet;
  // Update network Tx counters just before sending downstream.
  falcon_->get_stats_manager()->UpdateNetworkTxCounters(
      packet->packet_type, scid, is_retransmission,
      packet_metadata->retransmission_reason);
  VLOG(2) << "[" << falcon_->get_host_id() << ": "
          << falcon_->get_environment()->ElapsedTime() << "][" << scid << ", "
          << rsn << ", " << static_cast<int>(packet_type) << "] "
          << "Packet handed over to traffic shaper.";
  // 6. Send packet to the traffic shaper.
  falcon_->get_packet_metadata_transformer()->TransferTxPacket(
      std::move(packet_metadata->active_packet), scid);
  // Initialize active packet to a copy of the packet that is just transmitted,
  // to use for future retransmissions.
  packet_metadata->active_packet =
      std::make_unique<Packet>(packet_metadata->retransmission_packet_copy);
  return absl::OkStatus();
}

void ProtocolPacketReliabilityManager::PiggybackAck(uint32_t scid,
                                                    Packet* packet) {
  auto ack_coalescing_key = AckCoalescingKey(scid);
  CHECK_OK(falcon_->get_ack_coalescing_engine()->PiggybackAck(
      ack_coalescing_key, packet));
}

// Sets the PSN of the outgoing packet based on its packet type.
// Assigns SSN in case this is Push Req (SSNs for push grants are implicitly
// assigned when created by FALCON).
absl::Status ProtocolPacketReliabilityManager::AssignPsnAndSsn(
    ConnectionState* const connection_state,
    PacketMetadata* const packet_metadata, Packet* const packet) {
  // Set PSN of the packet to the NSN of the request or data window based on
  // packet type.
  auto* window = GetAppropriateTxWindow(
      &connection_state->tx_reliability_metadata, packet_metadata->type);
  packet->falcon.psn = window->next_packet_sequence_number;
  packet_metadata->psn = window->next_packet_sequence_number;
  window->next_packet_sequence_number += 1;
  if (packet_metadata->type == falcon::PacketType::kPushRequest) {
    packet->falcon.ssn = connection_state->tx_transaction_metadata
                             .current_solicitation_sequence_number++;
  }
  falcon_->get_stats_manager()->UpdateRequestOrDataWindowUsage(
      window->type, connection_state->connection_metadata->scid,
      window->next_packet_sequence_number -
          window->base_packet_sequence_number);
  return absl::OkStatus();
}

// Sets the various fields corresponding to an outgoing packet.
void ProtocolPacketReliabilityManager::SetOutgoingPacketHeader(
    ConnectionState* const connection_state, uint32_t rsn,
    falcon::PacketType type, Packet* const packet) {
  // Get a handle on the RX reliability metadata.
  ConnectionState::ReceiverReliabilityMetadata* const rx_reliability_metadata =
      &connection_state->rx_reliability_metadata;
  // Sets type of the packet.
  packet->packet_type = type;
  // Sets destination IP.
  CHECK_OK_THEN_ASSIGN(
      packet->metadata.destination_ip_address,
      Ipv6Address::OfString(connection_state->connection_metadata->dst_ip));
  // set bifurcation ids
  packet->metadata.source_bifurcation_id =
      connection_state->connection_metadata->source_bifurcation_id;
  packet->metadata.destination_bifurcation_id =
      connection_state->connection_metadata->destination_bifurcation_id;
  // Sets the traffic shaper appropriately (bypass only if fcwnd >= 1)
  uint32_t fcwnd = std::floor(falcon_rue::FixedToFloat<uint32_t, double>(
      connection_state->congestion_control_metadata->fabric_congestion_window,
      falcon_rue::kFractionalBits));
  if (fcwnd < 1) {
    packet->metadata.timing_wheel_timestamp =
        falcon_->get_environment()->ElapsedTime() +
        connection_state->congestion_control_metadata->inter_packet_gap;
  } else {
    packet->metadata.timing_wheel_timestamp = absl::ZeroDuration();
  }
  uint32_t old_flow_label = packet->metadata.flow_label;
  // Sets the flow label.
  packet->metadata.flow_label =
      ChooseOutgoingPacketFlowLabel(type, rsn, connection_state);
  AfterChooseOutgoingPacketFlowLabel(
      type, rsn, connection_state, old_flow_label, packet->metadata.flow_label);
  // Set FALCON related fields.
  //
  // support UD.
  packet->falcon.dest_cid = connection_state->connection_metadata->dcid;
  packet->falcon.rsn = rsn;
  // Set AR bit = 1 if fcwnd <= ar_threshold or with ar_percent probability.
  if (MeetsAckRequestedBitSetCriteria(fcwnd)) {
    packet->falcon.ack_req = true;
  } else if (packet->metadata.is_last_packet &&
             falcon_->get_config()->op_boundary_ar_bit().enable()) {
    // The token bucket rate limits only the acks due to op boundaries. All
    // other acks (via cwnd criteria or AR percent) don't consume tokens from
    // this token bucket.
    if (ack_token_bucket_.AreTokensAvailable(
            1, falcon_->get_environment()->ElapsedTime())) {
      ack_token_bucket_.RequestTokens(1);
      packet->falcon.ack_req = true;
    }
  }
  packet->falcon.rrbpsn = rx_reliability_metadata->request_window_metadata
                              .base_packet_sequence_number;
  packet->falcon.rdbpsn =
      rx_reliability_metadata->data_window_metadata.base_packet_sequence_number;

  switch (packet->packet_type) {
    case falcon::PacketType::kPullRequest:
      // RDMA read request, RDMA struct already contain the correct lengths.
      packet->falcon.request_length = packet->rdma.request_length;
      packet->falcon.payload_length = packet->rdma.data_length;
      break;
    case falcon::PacketType::kPushRequest:
    case falcon::PacketType::kPushGrant: {
      // FALCON induced packets, we set lengths according to transaction. There
      // is no RDMA payload, and all information is in FALCON headers, hence
      // total_payload_length set to 0.
      CHECK_OK_THEN_ASSIGN(
          auto transaction,
          connection_state->GetTransaction(TransactionKey(
              rsn, GetTransactionLocation(
                       /*type=*/packet->packet_type, /*incoming=*/false))));
      packet->falcon.request_length = transaction->request_length;
      packet->falcon.payload_length = 0;
      break;
    }
      // These are data packets sent by RDMA, so total payload length is set in
      // RDMA struct correctly. No need to set request length.
    case falcon::PacketType::kPullData:
      packet->falcon.payload_length = packet->rdma.response_length;
      break;
    case falcon::PacketType::kPushSolicitedData:
    case falcon::PacketType::kPushUnsolicitedData:
      packet->falcon.payload_length = packet->rdma.request_length;
      break;
    case falcon::PacketType::kAck:
    case falcon::PacketType::kNack:
    case falcon::PacketType::kBack:
    case falcon::PacketType::kEack:
    case falcon::PacketType::kInvalid:
      // Both falcon.request_length and payload_length are 0.
      break;
    case falcon::PacketType::kResync:
      LOG(FATAL) << "Resync logic is not supported.";
  }
}

// Sets up packet retransmission in the following manner -
// 1. Sets up a new retransmission timer (if appropriate)
// 2. Init packet retransmission state such as transmission time and
// transmission attempts.
// 3. Keep a record of the outstanding packet transmission (to trigger
// retransmission in the future).
absl::Status ProtocolPacketReliabilityManager::SetupRetransmission(
    ConnectionState* const connection_state,
    TransactionMetadata* const transaction,
    PacketMetadata* const packet_metadata) {
  // Initialize packet transmission metadata.
  packet_metadata->transmission_time =
      falcon_->get_environment()->ElapsedTime();
  if (packet_metadata->transmit_attempts > 1) {
    switch (packet_metadata->retransmission_reason) {
      case RetransmitReason::kTimeout:
      case RetransmitReason::kUlpRto:
        ++packet_metadata->timeout_retransmission_attempts;
        break;
      case RetransmitReason::kEarlyOooDis:
      case RetransmitReason::kEarlyRack:
      case RetransmitReason::kEarlyTlp:
      case RetransmitReason::kEarlyNack:
        ++packet_metadata->early_retx_attempts;
        break;
    }
  }
  falcon_->get_stats_manager()->UpdateMaxTransmissionCount(
      packet_metadata->transmit_attempts);
  // Record the fact that this packet is an outstanding packet.
  AddToOutstandingPackets(connection_state, transaction, packet_metadata);
  // Setup retransmission timer for connection (if appropriate).
  CHECK_OK(SetupRetransmitTimer(connection_state));
  return absl::OkStatus();
}

// Records the input packet as outstanding and changes the connection and
// transaction state accordingly.
void ProtocolPacketReliabilityManager::AddToOutstandingPackets(
    ConnectionState* const connection_state,
    TransactionMetadata* const transaction,
    PacketMetadata* const packet_metadata) {
  auto* window = GetAppropriateTxWindow(
      &connection_state->tx_reliability_metadata, packet_metadata->type);
  window->outstanding_packets.insert(RetransmissionWorkId(
      transaction->rsn, packet_metadata->psn, packet_metadata->type));
  // Add packet to outstanding packet contexts.
  window->outstanding_packet_contexts[packet_metadata->psn] =
      std::make_unique<OutstandingPacketContext>(transaction->rsn,
                                                 packet_metadata->type);
}

// Creates the received packet context.
void ProtocolPacketReliabilityManager::CreateReceivedPacketContext(
    absl::flat_hash_map<TransactionKey, std::unique_ptr<ReceivedPacketContext>>&
        recv_pkt_ctxs,
    const TransactionKey& key) {
  recv_pkt_ctxs.emplace(key, std::make_unique<ReceivedPacketContext>());
}

// Post-process (update related counters, etc.) the newly acked packet.
void ProtocolPacketReliabilityManager::AfterPacketAcked(
    ConnectionState* connection_state,
    const OutstandingPacketContext* packet_context) {
  AccumulateNumAcked(connection_state, packet_context);
}

// Accumulates the number of packets acked to be used for the congestion
// control algorithm in a RUE event.
void ProtocolPacketReliabilityManager::AccumulateNumAcked(
    ConnectionState* connection_state,
    const OutstandingPacketContext* packet_context) {
  connection_state->congestion_control_metadata->num_acked += 1;
}

// (Re-)schedule a retransmission timer corresponding to the minimum of the two
// heads of the two outstanding packet lists. No need to schedule if there is no
// outstanding packets, or an existing timer is already scheduled for the head.
// If the RTO for the head is already passed (could be caused by ACK removing
// some HoL packets, or RTO reduction), schedule the timer at Now.
absl::Status ProtocolPacketReliabilityManager::SetupRetransmitTimer(
    ConnectionState* const connection_state) {
  auto& tx_reliability_metadata = connection_state->tx_reliability_metadata;
  // Get the minimum of the RTO of the head of the two lists.
  absl::Duration base_time = absl::InfiniteDuration();
  std::array<absl::btree_set<RetransmissionWorkId>*, 2> lists = {
      &tx_reliability_metadata.request_window_metadata.outstanding_packets,
      &tx_reliability_metadata.data_window_metadata.outstanding_packets};
  for (auto list : lists) {
    if (!list->empty()) {
      auto packet_metadata =
          GetPacketMetadataFromWorkId(connection_state, *list->begin());
      auto candidate_base_time =
          packet_metadata->transmission_time +
          GetTimeoutOfPacket(connection_state, packet_metadata);
      if (candidate_base_time < base_time) base_time = candidate_base_time;
    }
  }
  // If no outstanding packets, no need to schedule.
  if (base_time == absl::InfiniteDuration()) return absl::OkStatus();
  base_time =
      absl::Nanoseconds(std::ceil(absl::ToDoubleNanoseconds(base_time)));
  // if there is already a timer with same base_time, no need to schedule.
  if (base_time == tx_reliability_metadata.rto_expiration_base_time)
    return absl::OkStatus();
  // If the head timeout is already passed (this could happen if ACK removes
  // some HoL pending packets, or RTO reduces), schedule the event at Now.
  if (base_time < falcon_->get_environment()->ElapsedTime()) {
    base_time = falcon_->get_environment()->ElapsedTime();
  }
  tx_reliability_metadata.rto_expiration_base_time = base_time;
  RETURN_IF_ERROR(falcon_->get_environment()->ScheduleEvent(
      base_time - falcon_->get_environment()->ElapsedTime(),
      [this, connection_state]() {
        HandleRetransmitTimeout(connection_state->connection_metadata->scid);
      }));
  return absl::OkStatus();
}

// Handles retransmission timeout in the following manner -
// 1. For each TX window - iterate through the
// outstanding PSNs and trigger retransmissions until we come across the first
// packet that does not meet the criteria (as we need to retransmit in PSN
// order).
// 2. Schedule the timer again if there are outstanding packets remaining.
void ProtocolPacketReliabilityManager::HandleRetransmitTimeout(uint32_t scid) {
  // Get a handle on the connection state.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       state_manager->PerformDirectLookup(scid));
  // If this is an invalid Timeout, return.
  if (!falcon_->get_environment()->ElapsedTimeEquals(
          connection_state->tx_reliability_metadata.GetRtoExpirationTime())) {
    return;
  }
  connection_state->tx_reliability_metadata.ClearRtoExpirationTime();

  // Get a handle on both the TX reliability windows and trigger
  // retransmissions.
  std::array<TransmitterReliabilityWindowMetadata*, 2> tx_windows{
      &connection_state->tx_reliability_metadata.request_window_metadata,
      &connection_state->tx_reliability_metadata.data_window_metadata};

  for (auto& tx_window : tx_windows) {
    while (!tx_window->outstanding_packets.empty()) {
      auto work_id = tx_window->outstanding_packets.begin();
      // Retransmission Eligibility : Ignore packet if already ACKed.
      int bitmap_index =
          (work_id->psn - tx_window->base_packet_sequence_number) %
          tx_window->window->Size();
      if (tx_window->window->Get(bitmap_index)) {
        tx_window->outstanding_packets.erase(work_id);
        continue;
      }
      // Get a handle on the packet metadata.
      auto packet_metadata =
          GetPacketMetadataFromWorkId(connection_state, *work_id);
      // Stop if we reach a PSN which has not timed out. Otherwise :
      // 1. Initiate retransmission if its below the retransmission threshold.
      // 2. If above, signal fatal error on the connection.
      auto packet_elapsed_time = falcon_->get_environment()->ElapsedTime() -
                                 packet_metadata->transmission_time;
      auto timeout = ProtocolPacketReliabilityManager::GetTimeoutOfPacket(
          connection_state, packet_metadata);
      // If timeout not expired, stop scanning.
      if (packet_elapsed_time < timeout) {
        break;
      }

      if (packet_metadata->timeout_retransmission_attempts >=
          connection_state->connection_metadata->max_retransmit_attempts) {
        tx_window->outstanding_packets.erase(work_id);
        LOG(FATAL) << "[" << falcon_->get_host_id() << ": "
                   << falcon_->get_environment()->ElapsedTime() << "][" << scid
                   << ", " << work_id->rsn << ", "
                   << static_cast<int>(work_id->type) << "] "
                   << "Packet has reached retranmission limit leading to "
                      "fatal connection error.";
      } else if (packet_metadata->received) {
        // If the packet is received, it has to be ULP-RTO.
        //
        packet_metadata->retransmission_reason = RetransmitReason::kUlpRto;
      } else {
        // It is an RTO.
        packet_metadata->retransmission_reason = RetransmitReason::kTimeout;
        // Reset these EACK-OWN related flags as an RTO period is over.
        packet_metadata->bypass_recency_check = true;
        packet_metadata->bypass_scanning_exit_criteria = false;
      }
      LogTimeout(falcon_->get_environment()->ElapsedTime(),
                 falcon_->get_host_id(), scid, *work_id,
                 packet_metadata->retransmission_reason);
      InitiateRetransmission(scid, work_id->rsn, work_id->type);
      tx_window->outstanding_packets.erase(work_id);
    }
  }

  // Try schedule a new RTO timer.
  CHECK_OK(SetupRetransmitTimer(connection_state));
}

absl::Duration ProtocolPacketReliabilityManager::GetTimeoutOfPacket(
    ConnectionState* const connection_state,
    PacketMetadata* const packet_metadata) {
  // If this packet is RNR-NACKed, always use RNR-TO.
  if (packet_metadata->nack_code == falcon::NackCode::kUlpReceiverNotReady)
    return packet_metadata->ulp_nack_metadata.rnr_timeout;
  // If this packet is received, only use ULP-RTO.
  if (packet_metadata->received) {
    return connection_state->ulp_rto;
  }

  return connection_state->congestion_control_metadata->retransmit_timeout;
}

// Get the packet metadata from rsn and type.
PacketMetadata* ProtocolPacketReliabilityManager::GetPacketMetadata(
    ConnectionState* const connection_state, uint32_t rsn,
    falcon::PacketType type) {
  // Get a handle on the transaction state.
  CHECK_OK_THEN_ASSIGN(auto transaction,
                       connection_state->GetTransaction(
                           TransactionKey(rsn, GetTransactionLocation(
                                                   /*type=*/type,
                                                   /*incoming=*/false))));
  absl::StatusOr<PacketMetadata*> packet_lookup_result =
      transaction->GetPacketMetadata(type);
  CHECK_OK(packet_lookup_result.status());
  return packet_lookup_result.value();
}

PacketMetadata* ProtocolPacketReliabilityManager::GetPacketMetadataFromPsn(
    ConnectionState* const connection_state,
    TransmitterReliabilityWindowMetadata* window, uint32_t psn) {
  CHECK_OK_THEN_ASSIGN(auto packet_context,
                       window->GetOutstandingPacketRSN(psn));
  return GetPacketMetadata(connection_state, packet_context->rsn,
                           packet_context->packet_type);
}

PacketMetadata* ProtocolPacketReliabilityManager::GetPacketMetadataFromWorkId(
    ConnectionState* const connection_state,
    const RetransmissionWorkId& work_id) {
  return GetPacketMetadata(connection_state, work_id.rsn, work_id.type);
}

// Handles RTO reduction by explicitly triggering retransmissions.
absl::Status ProtocolPacketReliabilityManager::HandleRtoReduction(
    uint32_t scid) {
  // If RTO is reduced, clear the existing retransmission timer.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(scid));
  connection_state->tx_reliability_metadata.ClearRtoExpirationTime();
  // Reschedule the timer (if RTO already passed, will schedule at Now).
  CHECK_OK(SetupRetransmitTimer(connection_state));
  return absl::OkStatus();
}

// Enqueues a packet in the retransmission scheduler for retransmission.
void ProtocolPacketReliabilityManager::InitiateRetransmission(
    uint32_t scid, uint32_t rsn, falcon::PacketType packet_type) {
  auto status = falcon_->get_retransmission_scheduler()->EnqueuePacket(
      scid, rsn, packet_type);

  // Get a handle on the transaction state and decrement ORRC accordingly.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(ConnectionState* const connection_state,
                       state_manager->PerformDirectLookup(scid));
  CHECK_OK_THEN_ASSIGN(auto transaction,
                       connection_state->GetTransaction(
                           TransactionKey(rsn, GetTransactionLocation(
                                                   /*type=*/packet_type,
                                                   /*incoming=*/false))));
  CHECK_OK_THEN_ASSIGN(auto packet_metadata,
                       transaction->GetPacketMetadata(packet_type));

  // Decrement ORRC in case this is > 1 retransmission of this packet.
  if (packet_metadata->transmit_attempts > 1 && status.ok()) {
    DecrementOutstandingRetransmittedRequestCount(scid, packet_type, false);
  }
}

// Handles early retransmission in the following manner. For the given window,
// trigger retransmissions for unacknowledged packets between BPSN and
// EarlyRetransmissionLimit. Ignore initiating retransmissions for packets
// already enqueued in connection scheduler and raise a fatal error if a packet
// exceed the retransmission limit.
absl::Status
ProtocolPacketReliabilityManager::InitiateNackBasedEarlyRetransmission(
    const Packet* packet) {
  uint32_t scid = packet->nack.dest_cid;
  bool is_request_window = packet->nack.request_window;
  // Get a handle on the connection state and the appropriate TX window.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(scid));
  // Get a handle on the appropriate TX window that needs to undergo early
  // retransmission.
  auto* tx_window = GetAppropriateTxWindow(
      &connection_state->tx_reliability_metadata, is_request_window);
  // Trigger retransmissions for the unacknowledged packets between BPSN and
  // BPSN + EarlyRetransmissionLimit.
  for (auto work_id = tx_window->outstanding_packets.begin();
       work_id != tx_window->outstanding_packets.end();) {
    int index = (work_id->psn - tx_window->base_packet_sequence_number) %
                tx_window->window->Size();
    // Stop when beyond BPSN + EarlyRetransmissionLimit.
    if (index >= connection_state->connection_metadata->early_retransmit_limit)
      break;
    if (!tx_window->window->Get(index)) {
      // Get a handle on the packet metadata.
      auto const packet_metadata =
          GetPacketMetadataFromWorkId(connection_state, *work_id);
      // Recency check: if the last tx is >= instant_rtt ago.
      auto instant_rtt =
          packet->timestamps.received_timestamp - packet->nack.timestamp_1;
      if (packet_metadata->transmission_time + instant_rtt >=
          falcon_->get_environment()->ElapsedTime()) {
        // Stop scanning when a PSN fails recency check.
        break;
      }
      // Only early-retx if below early retransmission threshold.
      if (packet_metadata->early_retx_attempts < early_retx_threshold_) {
        packet_metadata->retransmission_reason = RetransmitReason::kEarlyNack;
        InitiateRetransmission(scid, work_id->rsn, work_id->type);
        work_id = tx_window->outstanding_packets.erase(work_id);
      } else {
        break;
      }
    }
  }
  return absl::OkStatus();
}

// Responsible for receiving an incoming packet and performing the
// sliding-window related processing.
absl::Status ProtocolPacketReliabilityManager::ReceivePacket(
    const Packet* packet) {
  switch (packet->packet_type) {
    case falcon::PacketType::kPullRequest:
    case falcon::PacketType::kPullData:
    case falcon::PacketType::kPushRequest:
    case falcon::PacketType::kPushGrant:
    case falcon::PacketType::kPushSolicitedData:
    case falcon::PacketType::kPushUnsolicitedData:
      return HandleIncomingPacket(packet);
    case falcon::PacketType::kResync:
      LOG(FATAL) << "Resync logic is not supported.";
    case falcon::PacketType::kAck:
      return HandleACK(packet);
    case falcon::PacketType::kNack:
      return HandleNack(packet);
    case falcon::PacketType::kBack:
    case falcon::PacketType::kEack:
      //
      LOG(FATAL) << "Isekai doesn't use EACK/BACK packet type yet.";
      break;
    case falcon::PacketType::kInvalid:
      LOG(FATAL) << "Received an invalid packet.";
  }
}

// Responsible for performing sliding window checks for the incoming packet.
// It does so in the following manner -
// 1. PSN < BPSN: (a) Generate explicit ACK or update ACK coalescing state and
// (b) Drop duplicate packet.
// 2. PSN >= BPSN + Window Size : Generate NACK packet, drop duplicate packet.
// 3. BPSN <= PSN < BPSN + Window Size, then -
// 3.1 Window[PSN] is set: Drop duplicate packet.
// 3.2 PSN == BPSN: If packet does not correspond to push data, then (a)
// increment BPSN to oldest unACKed packet and shift the window as well, and (b)
// generate explicit ACK or update ACK coalescing state. Also, update the
// relevant bitmaps. In case of a duplicate packet, apart from dropping the
// packet, also generate an explicit ACK or update ACK coalescing state, if
// packet is already ACK-ed.
absl::Status ProtocolPacketReliabilityManager::HandleIncomingPacket(
    const Packet* packet) {
  // Get a handle on the connection state.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  uint32_t scid = packet->falcon.dest_cid;
  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(scid));
  // Get the appropriate RX window metadata.
  auto* rx_window_metadata = GetAppropriateRxWindow(
      &connection_state->rx_reliability_metadata, packet->packet_type);
  LogPacket(falcon_->get_environment()->ElapsedTime(), falcon_->get_host_id(),
            packet, scid, false);
  uint32_t psn = packet->falcon.psn;
  // Get a handle on the ack coalescing engine.
  AckCoalescingEngineInterface* ack_coalescing_engine =
      falcon_->get_ack_coalescing_engine();
  auto ack_coalescing_key =
      ack_coalescing_engine->GenerateAckCoalescingKeyFromIncomingPacket(packet);
  // Record target congestion control metadata reflected in the incoming packet.
  CHECK_OK(ack_coalescing_engine->UpdateCongestionControlMetadataToReflect(
      *ack_coalescing_key, packet));
  if (SeqLT(psn, rx_window_metadata->base_packet_sequence_number)) {
    // PSN < BPSN: (a) Generate ACK or update coalescing state.
    CHECK_OK(ack_coalescing_engine->GenerateAckOrUpdateCoalescingState(
        std::move(ack_coalescing_key), packet->falcon.ack_req, 2));
    // PSN < BPSN: (b) Drop duplicate packet.
    return absl::AlreadyExistsError("Duplicate packet. Dropped.");
  } else if (SeqGEQ(psn, (rx_window_metadata->base_packet_sequence_number +
                          rx_window_metadata->receive_window->Size()))) {
    if (!EackBasedEarlyRetransmissionIsEnabled()) {
      // PSN >= BPSN + Window Size : Generate NACK packet, drop duplicate
      // packet.
      CHECK_OK(ack_coalescing_engine->TransmitNACK(
          *ack_coalescing_key, psn, BelongsToRequestWindow(packet->packet_type),
          falcon::NackCode::kReserved, nullptr));
    } else if (enable_eack_own_) {
      // Update the window state to indicate that its experienced sliding window
      // drops and update the ACK coalescing state. Sliding window drop packets
      // increment the ACK coalescing counter by 2.
      rx_window_metadata->has_oow_drop = true;
      CHECK_OK(ack_coalescing_engine->GenerateAckOrUpdateCoalescingState(
          std::move(ack_coalescing_key), packet->falcon.ack_req, 2));
    }
    return absl::OutOfRangeError(
        "Received packet is beyond the siliding window capacity.");
  } else {
    uint32_t bitmap_index =
        (psn - rx_window_metadata->base_packet_sequence_number) %
        rx_window_metadata->receive_window->Size();
    if (rx_window_metadata->receive_window->Get(bitmap_index)) {
      CHECK_OK(ack_coalescing_engine->GenerateAckOrUpdateCoalescingState(
          std::move(ack_coalescing_key), packet->falcon.ack_req, 2));
      CHECK_OK(HandleDuplicateNackedPacket(connection_state, packet));
      return absl::AlreadyExistsError("Duplicate packet. Dropped.");
    } else {
      // Check resource availability corresponding to the packet.
      auto resource_availability =
          falcon_->get_resource_manager()
              ->VerifyResourceAvailabilityOrReserveResources(
                  scid, packet, PacketDirection::kIncoming, false);
      if (resource_availability.ok()) {
        HandleImplicitAck(packet, connection_state);
        //  Update receive window to prevent duplicates.
        rx_window_metadata->receive_window->Set(bitmap_index, true);
        if ((packet->packet_type != falcon::PacketType::kPushSolicitedData) &&
            (packet->packet_type != falcon::PacketType::kPushUnsolicitedData)) {
          //  Update ack window rightaway (need not wait for ULP as the packet
          //  is not Push Data).
          UpdateReceiveAckBitmap(&connection_state->rx_reliability_metadata,
                                 rx_window_metadata, psn);
          // Generate ACK or update coalescing state.
          CHECK_OK(ack_coalescing_engine->GenerateAckOrUpdateCoalescingState(
              std::move(ack_coalescing_key), packet->falcon.ack_req, 2));
        } else {
          // If push data: Generate ACK or update coalescing state. AR should
          // not be used for push data at receive time.
          RETURN_IF_ERROR(
              ack_coalescing_engine->GenerateAckOrUpdateCoalescingState(
                  std::move(ack_coalescing_key), false, 1));
        }
        // Create/update the context of the transaction corresponding to the
        // received packet (leaving resyncs).
        if (packet->packet_type != falcon::PacketType::kResync) {
          CreateOrUpdateReceiverPacketContext(connection_state, packet,
                                              PacketDirection::kIncoming);
        }
        HandlePiggybackedACK(packet, connection_state);
        return absl::OkStatus();
      } else {
        // Handle piggybacked ACK information even if packet cannot be admitted
        // due to no resource availability.
        HandlePiggybackedACK(packet, connection_state);
        return resource_availability;
      }
    }
  }
}

// Updates the receiver ACK bitmap. This is done on receiving an ACK from ULP
// for push data packets and immediately (on receiving) for others.
void ProtocolPacketReliabilityManager::UpdateReceiveAckBitmap(
    ConnectionState::ReceiverReliabilityMetadata* const rx_reliability_metadata,
    ReceiverReliabilityWindowMetadata* const rx_window_metadata, uint32_t psn) {
  uint32_t bitmap_index =
      (psn - rx_window_metadata->base_packet_sequence_number) %
      rx_window_metadata->receive_window->Size();
  // Update the receiver ACK bitmap.
  rx_window_metadata->ack_window->Set(bitmap_index, true);
  // PSN == BPSN: (b) Increment BPSN to oldest unACKed packet and shift
  // the ACK and receive windows as well.
  if (psn == rx_window_metadata->base_packet_sequence_number) {
    uint32_t base_psn_shift_delta = 0;
    for (auto i = 0; i < rx_window_metadata->ack_window->Size(); i++) {
      if (!rx_window_metadata->ack_window->Get(i)) {
        break;
      }
      // Update base_psn_shift_delta to the index of last '1' plus 1.
      base_psn_shift_delta = i + 1;
    }
    rx_window_metadata->ack_window->RightShift(base_psn_shift_delta);
    rx_window_metadata->receive_window->RightShift(base_psn_shift_delta);
    rx_window_metadata->base_packet_sequence_number += base_psn_shift_delta;
    rx_reliability_metadata->implicitly_acked_counter += base_psn_shift_delta;
  }
}

// Creates/updates the context of the packet received. Changes done whenever a
// packet is being sent or received.
void ProtocolPacketReliabilityManager::CreateOrUpdateReceiverPacketContext(
    ConnectionState* const connection_state, const Packet* packet,
    PacketDirection direction) {
  // Get a handle of receiver reliability metadata
  auto& recv_pkt_ctxs =
      connection_state->rx_reliability_metadata.received_packet_contexts;
  // Create the received packet context, if it does not exist using emplace
  // API.
  uint32_t rsn = packet->falcon.rsn;
  TransactionLocation location;
  if (direction == PacketDirection::kIncoming) {
    location = GetTransactionLocation(/*type=*/packet->packet_type,
                                      /*incoming=*/true);
  } else {
    location = GetTransactionLocation(/*type=*/packet->packet_type,
                                      /*incoming=*/false);
  }
  const TransactionKey pkt_ctxs_key(rsn, location);
  CreateReceivedPacketContext(recv_pkt_ctxs, pkt_ctxs_key);
  auto recv_pkt_ctx = recv_pkt_ctxs[pkt_ctxs_key].get();
  // Initialize the metadata from the packet being received or sent out.
  if (direction == PacketDirection::kIncoming) {
    recv_pkt_ctx->psn = packet->falcon.psn;
    recv_pkt_ctx->type = packet->packet_type;
    recv_pkt_ctx->is_ack_requested = packet->falcon.ack_req;
    // Initialize QP ID if it corresponds to packets that have the RDMA
    // header.
    if (packet->packet_type == falcon::PacketType::kPushSolicitedData ||
        packet->packet_type == falcon::PacketType::kPushUnsolicitedData ||
        packet->packet_type == falcon::PacketType::kPushRequest ||
        packet->packet_type == falcon::PacketType::kPullRequest ||
        packet->packet_type == falcon::PacketType::kPullData) {
      recv_pkt_ctx->qp_id = packet->rdma.dest_qp_id;
    }
  } else {
    if (packet->packet_type == falcon::PacketType::kPushSolicitedData ||
        packet->packet_type == falcon::PacketType::kPushUnsolicitedData ||
        packet->packet_type == falcon::PacketType::kPushRequest ||
        packet->packet_type == falcon::PacketType::kPullRequest ||
        packet->packet_type == falcon::PacketType::kPullData) {
      // Only QP ID is initialized as other fields are initialized when a
      // packet is received.
      recv_pkt_ctx->qp_id = packet->metadata.rdma_src_qp_id;
    }
  }
}

// Handles receiving duplicate NACK-ed packets.
absl::Status ProtocolPacketReliabilityManager::HandleDuplicateNackedPacket(
    ConnectionState* const connection_state, const Packet* packet) {
  if (packet->packet_type == falcon::PacketType::kPushSolicitedData ||
      packet->packet_type == falcon::PacketType::kPushUnsolicitedData) {
    // Get a handle on the packet metadata to see if this is an already NACK-ed
    // packet.
    auto status_or = connection_state->GetTransaction(TransactionKey(
        packet->falcon.rsn, GetTransactionLocation(/*type=*/packet->packet_type,
                                                   /*incoming=*/true)));
    TransactionMetadata* transaction;
    if (status_or.ok()) {
      transaction = status_or.value();
    } else if (status_or.status().code() == absl::StatusCode::kNotFound) {
      // If not found, this transaction is ULP-Acked, so no need to
      // check nack_code.
      return absl::OkStatus();
    } else {
      return status_or.status();
    }
    ASSIGN_OR_RETURN(PacketMetadata* const packet_metadata,
                     transaction->GetPacketMetadata(packet->packet_type));
    // Get a handle on the ack coalescing engine.
    AckCoalescingEngineInterface* ack_coalescing_engine =
        falcon_->get_ack_coalescing_engine();
    auto ack_coalescing_key =
        ack_coalescing_engine->GenerateAckCoalescingKeyFromIncomingPacket(
            packet);
    switch (packet_metadata->nack_code) {
      case falcon::NackCode::kUlpReceiverNotReady:
        // Check with reorder engine. If needed, send RNR NACK.
        if (falcon_->get_buffer_reorder_engine()
                ->RetryRnrNackedPacket(packet->falcon.dest_cid,
                                       packet->falcon.rsn)
                .code() == absl::StatusCode::kFailedPrecondition)
          CHECK_OK(ack_coalescing_engine->TransmitNACK(
              *ack_coalescing_key, packet->falcon.psn,
              BelongsToRequestWindow(packet->packet_type),
              packet_metadata->nack_code, &packet_metadata->ulp_nack_metadata));
        break;
      case falcon::NackCode::kUlpCompletionInError:
      case falcon::NackCode::kUlpNonRecoverableError:
      case falcon::NackCode::kInvalidCidError:
        // Regenerate NACK packet and send it to initiator.
        CHECK_OK(ack_coalescing_engine->TransmitNACK(
            *ack_coalescing_key, packet->falcon.psn,
            BelongsToRequestWindow(packet->packet_type),
            packet_metadata->nack_code, &packet_metadata->ulp_nack_metadata));
        break;
      case falcon::NackCode::kXlrDrop:
      case falcon::NackCode::kNotANack:
      case falcon::NackCode::kRxResourceExhaustion:
      case falcon::NackCode::kReserved:
        break;
    }
  }
  return absl::OkStatus();
}

// Handles an incoming ACK packet in the following manner. If an RX window
// BPSN in ACK < TX window BPSN: Discard ACK. Else, for each window -
// 2. RX window BPSN in ACK > TX window BPSN: Update TX window BPSN with ACK
// RX window BPSN.
// 3. Free up resources allocated to all ACK-ed packets in the retransmit
// buffer.
// 4. Send out ACK event to the RUE.
// 5. Check if any packets need to be retransmitted (as the head of the
// retransmission list may change depending on the packets being ACKed).
absl::Status ProtocolPacketReliabilityManager::HandleACK(const Packet* packet) {
  // Get a handle on the connection state.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  uint32_t scid = packet->ack.dest_cid;
  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(scid));
  LogPacket(falcon_->get_environment()->ElapsedTime(), falcon_->get_host_id(),
            packet, scid, false);

  // Get a handle on the TX windows' metadata.
  std::array<TransmitterReliabilityWindowMetadata*, 2> tx_windows_metadata{
      &connection_state->tx_reliability_metadata.request_window_metadata,
      &connection_state->tx_reliability_metadata.data_window_metadata};

  // Get the BPSN from the incoming ACK, per window.
  std::array<uint32_t, 2> ack_bpsns{packet->ack.rrbpsn, packet->ack.rdbpsn};

  // A RX window BPSN in ACK < TX window BPSN: Stale ACK.
  if (SeqLT(ack_bpsns[0],
            tx_windows_metadata[0]->base_packet_sequence_number) ||
      SeqLT(ack_bpsns[1],
            tx_windows_metadata[1]->base_packet_sequence_number)) {
    return HandleStaleAck(packet);
  }

  // Iterate through the request and data window, and update TX window state.
  for (int i = 0; i < tx_windows_metadata.size(); i++) {
    auto& tx_window_metadata = tx_windows_metadata[i];
    auto ack_bpsn = ack_bpsns[i];
    auto& slack = (i == 0) ? request_window_slack_ : data_window_slack_;
    auto& own_notification =
        i == 0 ? packet->ack.request_own : packet->ack.data_own;
    // RX window BPSN in ACK > TX window BPSN: Update TX window BPSN with ACK
    // RX window BPSN and OOW notification bit.
    uint32_t base_psn_delta = 0;
    if (SeqGT(ack_bpsn, tx_window_metadata->base_packet_sequence_number)) {
      // Unset the OOW notification received bit in case the difference between
      // the next-psn and base-psn < slack.
      if (SeqGEQ(ack_bpsn + slack,
                 tx_window_metadata->next_packet_sequence_number)) {
        tx_window_metadata->oow_notification_received = false;
      } else if (own_notification) {
        tx_window_metadata->oow_notification_received = true;
      }
      // Update TX window BPSN with ACK RX window BPSN.
      base_psn_delta =
          ack_bpsn - tx_window_metadata->base_packet_sequence_number;
      for (int index = 0; index < base_psn_delta; ++index) {
        if (!tx_window_metadata->window->Get(index)) {
          EnqueueAckToUlp(
              tx_window_metadata, scid,
              index + tx_window_metadata->base_packet_sequence_number);
        }
      }
      tx_window_metadata->window->RightShift(base_psn_delta);
      tx_window_metadata->base_packet_sequence_number = ack_bpsn;
      falcon_->get_stats_manager()->UpdateRequestOrDataWindowUsage(
          tx_window_metadata->type, connection_state->connection_metadata->scid,
          tx_window_metadata->next_packet_sequence_number -
              tx_window_metadata->base_packet_sequence_number);
    }
  }
  bool drop_detected = false;
  if (packet->ack.ack_type == Packet::Ack::kEack) {
    auto status = HandleEACK(packet, drop_detected);
    if (!status.ok()) {
      return status;
    }
  }
  falcon_->get_rate_update_engine()->ExplicitAckReceived(
      packet, packet->ack.ack_type == Packet::Ack::kEack, drop_detected);
  // ACK may remove some HoL outstanding packet, so reschedule RTO.
  CHECK_OK(SetupRetransmitTimer(connection_state));
  return absl::OkStatus();
}

// Handles a stale ACK where RX window BPSN in ACK < TX window BPSN.
absl::Status ProtocolPacketReliabilityManager::HandleStaleAck(
    const Packet* ack_packet) {
  // Discard stale ACK.
  return absl::AlreadyExistsError("Duplicate ACK. Dropped.");
}

void ProtocolPacketReliabilityManager::HandleImplicitAck(
    const Packet* packet, ConnectionState* const connection_state) {
  uint32_t scid = packet->falcon.dest_cid;
  uint32_t rsn = packet->falcon.rsn;
  uint32_t tx_psn;
  TransmitterReliabilityWindowMetadata* tx_window_metadata;
  // Fetch the appropriate packet PSN and tx_window.
  switch (packet->packet_type) {
    case falcon::PacketType::kPullData: {
      CHECK_OK_THEN_ASSIGN(auto transaction,
                           connection_state->GetTransaction(
                               {rsn, TransactionLocation::kInitiator}));
      CHECK_OK_THEN_ASSIGN(
          PacketMetadata* const packet_metadata,
          transaction->GetPacketMetadata(falcon::PacketType::kPullRequest));

      tx_psn = packet_metadata->psn;
      tx_window_metadata =
          &connection_state->tx_reliability_metadata.request_window_metadata;
      break;
    }
    case falcon::PacketType::kPushSolicitedData: {
      CHECK_OK_THEN_ASSIGN(auto transaction,
                           connection_state->GetTransaction(
                               {rsn, TransactionLocation::kTarget}));
      CHECK_OK_THEN_ASSIGN(
          PacketMetadata* const packet_metadata,
          transaction->GetPacketMetadata(falcon::PacketType::kPushGrant));

      tx_psn = packet_metadata->psn;
      tx_window_metadata =
          &connection_state->tx_reliability_metadata.data_window_metadata;
      break;
    }
    case falcon::PacketType::kPushGrant: {
      CHECK_OK_THEN_ASSIGN(auto transaction,
                           connection_state->GetTransaction(
                               {rsn, TransactionLocation::kInitiator}));
      CHECK_OK_THEN_ASSIGN(
          PacketMetadata* const packet_metadata,
          transaction->GetPacketMetadata(falcon::PacketType::kPushRequest));

      tx_psn = packet_metadata->psn;
      tx_window_metadata =
          &connection_state->tx_reliability_metadata.request_window_metadata;
      break;
    }
    default:
      return;
      // Do nothing.
  }

  // If the corresponding request or grant has been acked already, do nothing.
  if (tx_psn < tx_window_metadata->base_packet_sequence_number) {
    return;
  }

  // Find the packet index of the corresponding request/grant in the tx window.
  uint32_t tx_packet_index =
      tx_psn - tx_window_metadata->base_packet_sequence_number;

  // If the packet has not been acked, perform the ack processing.
  if (!tx_window_metadata->window->Get(tx_packet_index)) {
    EnqueueAckToUlp(tx_window_metadata, scid, tx_psn);
    tx_window_metadata->window->Set(tx_packet_index, true);

    // If the implicitly acked packet was the earliest unacked packet, then
    // right shift the window by 1.
    if (tx_psn == tx_window_metadata->base_packet_sequence_number) {
      tx_window_metadata->window->RightShift(1);
      ++tx_window_metadata->base_packet_sequence_number;
    }
    // HoL outstanding packet may move or change timeout by RACK, so reschedule.
    CHECK_OK(SetupRetransmitTimer(connection_state));
  }
}

// Handles piggybacked ACKs by adjusting the TX BPSN based on RX BPSN. ACK
// information is piggybacked on packets as well as NACKs.
void ProtocolPacketReliabilityManager::HandlePiggybackedACK(
    const Packet* packet, ConnectionState* connection_state) {
  uint32_t scid = packet->falcon.dest_cid;
  if (packet->packet_type == falcon::PacketType::kNack) {
    scid = packet->nack.dest_cid;
  }

  // Get a handle on the TX windows' metadata.
  std::array<TransmitterReliabilityWindowMetadata*, 2> tx_windows_metadata{
      &connection_state->tx_reliability_metadata.request_window_metadata,
      &connection_state->tx_reliability_metadata.data_window_metadata};

  // Get a handle on the incoming ACK metadata, window-wise.
  std::array<uint32_t, 2> ack_windows_bpsn;
  if (packet->packet_type == falcon::PacketType::kNack) {
    ack_windows_bpsn = {packet->nack.rrbpsn, packet->nack.rdbpsn};
  } else {
    ack_windows_bpsn = {packet->falcon.rrbpsn, packet->falcon.rdbpsn};
  }

  // A RX window BPSN in ACK < TX window BPSN: Discard ACK.
  if (SeqLT(ack_windows_bpsn[0],
            tx_windows_metadata[0]->base_packet_sequence_number) ||
      SeqLT(ack_windows_bpsn[1],
            tx_windows_metadata[1]->base_packet_sequence_number)) {
    return;
  }

  // Iterate through the request and data window, and update TX window state.
  for (int i = 0; i < tx_windows_metadata.size(); i++) {
    auto& tx_window_bpsn = tx_windows_metadata[i]->base_packet_sequence_number;
    auto& ack_window_bpsn = ack_windows_bpsn[i];
    auto& slack = (i == 0) ? request_window_slack_ : data_window_slack_;
    auto& own_notification =
        i == 0 ? packet->nack.request_own : packet->nack.data_own;
    // RX window BPSN in ACK > TX window BPSN: Update TX window BPSN with ACK
    // RX window BPSN and OOW notification bit.
    uint32_t base_psn_delta = 0;
    if (SeqGT(ack_window_bpsn, tx_window_bpsn)) {
      // Unset the OOW notification received bit in case the difference between
      // the next-psn and base-psn < slack.
      if (SeqGEQ(ack_window_bpsn + slack,
                 tx_windows_metadata[i]->next_packet_sequence_number)) {
        tx_windows_metadata[i]->oow_notification_received = false;
      } else if (own_notification) {
        tx_windows_metadata[i]->oow_notification_received = true;
      }
      base_psn_delta = ack_window_bpsn - tx_window_bpsn;
      for (int index = 0; index < base_psn_delta; ++index) {
        if (!tx_windows_metadata[i]->window->Get(index)) {
          EnqueueAckToUlp(tx_windows_metadata[i], scid, index + tx_window_bpsn);
        }
      }
      tx_windows_metadata[i]->window->RightShift(base_psn_delta);
      tx_window_bpsn = ack_window_bpsn;
    }
  }
  // ACK may remove some HoL outstanding packet, so reschedule RTO.
  CHECK_OK(SetupRetransmitTimer(connection_state));
}

// This function handle E-ACK. Specifically, it checks the ack bitmap and
// acknowledges packets, and checks the received bitmap, and uses the configured
// algorithm to decide which packets to retransmit.
absl::Status ProtocolPacketReliabilityManager::HandleEACK(const Packet* packet,
                                                          bool& drop_detected) {
  CHECK_EQ(packet->ack.ack_type, Packet::Ack::kEack);
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  uint32_t scid = packet->ack.dest_cid;
  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(scid));

  // Get a handle on both the TX reliability windows.
  std::array<TransmitterReliabilityWindowMetadata*, 2> tx_windows{
      &connection_state->tx_reliability_metadata.request_window_metadata,
      &connection_state->tx_reliability_metadata.data_window_metadata};
  // Get a handle on the incoming ACK metadata, window-wise.
  std::array<AckWindowMetadata, 2> ack_windows_metadata{
      AckWindowMetadata(packet->ack.rrbpsn, packet->ack.request_own,
                        packet->ack.receiver_request_bitmap),
      AckWindowMetadata(packet->ack.rdbpsn, packet->ack.data_own,
                        packet->ack.receiver_data_bitmap),
  };
  // Iterate through the request and data window, and update TX window state.
  for (int i = 0; i < tx_windows.size(); i++) {
    auto& tx_window_metadata = tx_windows[i];
    auto& ack_window_metadata = ack_windows_metadata[i];
    // Update TX window by OR-ing with aligned RX window.
    for (int index = 0; index < ack_window_metadata.window.Size(); index++) {
      if (!tx_window_metadata->window->Get(index) &&
          (ack_window_metadata.window.Get(index))) {
        EnqueueAckToUlp(
            tx_window_metadata, scid,
            tx_window_metadata->base_packet_sequence_number + index);
        tx_window_metadata->window->Set(index, true);
      }
    }
  }

  std::array<AckWindowMetadata, 2> ack_received_metadata{
      AckWindowMetadata(packet->ack.rrbpsn, packet->ack.request_own,
                        packet->ack.receiver_request_bitmap),
      AckWindowMetadata(packet->ack.rdbpsn, packet->ack.data_own,
                        packet->ack.received_bitmap),
  };
  if (EackBasedEarlyRetransmissionIsEnabled()) {
    // Early retransmission.
    std::vector<int> retx_limit(2, -1);
    if (enable_ooo_count_) {
      auto algo_result = EarlyRetransmissionOooCount(ack_received_metadata);
      EarlyRetransmissionMergeRetxLimit(retx_limit, algo_result);
    }
    if (enable_ooo_distance_) {
      auto algo_result = EarlyRetransmissionOooDistance(ack_received_metadata);
      EarlyRetransmissionMergeRetxLimit(retx_limit, algo_result);
    }
    // In case of receiving EACK-OWN, the retx limit should be the last PSN sent
    // out on the corresponding window.
    if (enable_eack_own_) {
      for (int window_index = 0; window_index < 2; window_index++) {
        auto& ack_metadata = ack_received_metadata[window_index];
        auto& tx_window_metadata = tx_windows[window_index];
        if (ack_metadata.own) {
          retx_limit[window_index] =
              tx_window_metadata->next_packet_sequence_number - 1;
        }
      }
    }

    // Set eack_drop (for RUE event): if some holes pass the check above.
    for (int window_index = 0; window_index < 2 && !drop_detected;
         window_index++) {
      auto& ack_metadata = ack_received_metadata[window_index];
      int end_of_scan =
          std::min(ack_metadata.window.Size() - 1, retx_limit[window_index]);
      for (int i = 0; i <= end_of_scan && !drop_detected; i++)
        if (!ack_metadata.window.Get(i)) {
          drop_detected = true;
        }
    }

    // Do actual retransmission.
    for (int window_index = 0; window_index < 2; window_index++) {
      auto& tx_metadata = tx_windows[window_index];
      auto& ack_metadata = ack_received_metadata[window_index];
      bool force_recency_check_bypass = false;

      // Early-retransmit outstanding packets before highest_psn_to_retx.
      for (auto work_id = tx_metadata->outstanding_packets.begin();
           work_id != tx_metadata->outstanding_packets.end();) {
        int bitmap_index =
            (work_id->psn - tx_metadata->base_packet_sequence_number) %
            tx_metadata->window->Size();
        // Stop when there is not enough OOO after this PSN.
        if (bitmap_index > retx_limit[window_index]) {
          break;
        }
        if (ShouldSkipRetransmission(bitmap_index, ack_metadata, tx_metadata,
                                     packet)) {
          ++work_id;
          continue;
        }
        // Get a handle on the packet metadata.
        auto const packet_metadata =
            GetPacketMetadataFromWorkId(connection_state, *work_id);
        // If this packet is RNR-NACKed, stop.
        if (packet_metadata->nack_code ==
            falcon::NackCode::kUlpReceiverNotReady) {
          break;
        }
        // Compute the packets instantaneous RTT.
        auto instant_rtt =
            packet->timestamps.received_timestamp - packet->ack.timestamp_1;

        // This packet can be early retransmitted when:
        // 1. Recency check for the packet passes, i.e., last tx time >= RTT.
        // 2. Received EACK-OWN and packet can bypass recency check.
        // 3. Received EACK-OWN and packet's recency check is assumed to pass.
        auto recency_check_pass = falcon_->get_environment()->ElapsedTime() -
                                      packet_metadata->transmission_time >=
                                  instant_rtt;
        auto recency_check_bypass = enable_recency_check_bypass_ &&
                                    ack_metadata.own &&
                                    packet_metadata->bypass_recency_check;
        auto smaller_psn_recency_check_bypass =
            enable_smaller_psn_recency_check_bypass_ && ack_metadata.own &&
            force_recency_check_bypass;

        if (recency_check_pass || recency_check_bypass ||
            smaller_psn_recency_check_bypass) {
          // Stop simulation in case we see unexpected retransmissions.
          if (packet_metadata->early_retx_attempts >= early_retx_threshold_) {
            LOG(FATAL) << "[" << falcon_->get_host_id() << ": "
                       << falcon_->get_environment()->ElapsedTime() << "]["
                       << scid << ", " << work_id->rsn << ", "
                       << static_cast<int>(work_id->type) << "] "
                       << "Packet has reached early retranmission limit. "
                          "Unexpected behavior.";
          }
          packet_metadata->retransmission_reason =
              RetransmitReason::kEarlyOooDis;
          // Disallow bypassing receny check for the packet in this RTO period
          // as its being early retransmitted.
          packet_metadata->bypass_recency_check = false;
          // In case this packet is being retransmitted due to EACK-OWN,
          // additional flags need to be updated.
          if (ack_metadata.own) {
            // Allow recency check bypass for higher PSN packets during this
            // scanning iteration.
            force_recency_check_bypass = true;
            // Do not allow this packet to bypass scanning exit criteria.
            packet_metadata->bypass_scanning_exit_criteria = false;
          } else {
            // Allow this packet to bypass scanning exit criteria in the future
            // as it was retransmitted while handling EACK.
            packet_metadata->bypass_scanning_exit_criteria = true;
          }
          InitiateRetransmission(scid, work_id->rsn, work_id->type);
          work_id = tx_metadata->outstanding_packets.erase(work_id);
        } else if (enable_scanning_exit_criteria_bypass_ && ack_metadata.own &&
                   packet_metadata->bypass_scanning_exit_criteria) {
          // Continue scanning if this is an EACK-OWN and the packet is eligible
          // to bypass scanning exit criteria.
          work_id++;
          continue;
        } else {
          // Otherwise, stop scanning.
          break;
        }
      }
    }
  }
  return absl::OkStatus();
}

// This function returns whether a packet should skip retransmission.
bool ProtocolPacketReliabilityManager::ShouldSkipRetransmission(
    int bitmap_index, const AckWindowMetadata& ack_metadata,
    const TransmitterReliabilityWindowMetadata* tx_metadata,
    const Packet* eack_packet) {
  // Skip packet that is received.
  if ((bitmap_index < ack_metadata.window.Size()) &&
      (ack_metadata.window.Get(bitmap_index))) {
    return true;
  }
  return false;
}

// Handles incoming NACK in the following manner -
// 1. Process piggybacked ACK information.
// 2. Perform NACK type handling as -
// (a) kReserved causes early retransmissions.
// (b) kRNR leads to the RNR logic being applied.
// (c) Other NACKs result in resync process which is not modeled yet.
// Also, all NACKs interact with the RUE.
absl::Status ProtocolPacketReliabilityManager::HandleNack(
    const Packet* packet) {
  LogPacket(falcon_->get_environment()->ElapsedTime(), falcon_->get_host_id(),
            packet, packet->nack.dest_cid, false);
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  uint32_t scid = packet->nack.dest_cid;
  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(scid));

  // Process piggybacked ACK information.
  HandlePiggybackedACK(packet, connection_state);
  // Specific handling logic per NACK type.
  switch (packet->nack.code) {
    case falcon::NackCode::kRxResourceExhaustion:
      break;
    case falcon::NackCode::kUlpReceiverNotReady:
      CHECK_OK(HandleRnrNack(packet));
      break;
    case falcon::NackCode::kReserved:
      // If EACK-based early retransmission is not enable, do NACK-based early
      // retransmission.
      if (!EackBasedEarlyRetransmissionIsEnabled()) {
        CHECK_OK(InitiateNackBasedEarlyRetransmission(packet));
      }
      break;
    case falcon::NackCode::kUlpCompletionInError:
    case falcon::NackCode::kUlpNonRecoverableError:
    case falcon::NackCode::kInvalidCidError:
    case falcon::NackCode::kXlrDrop:
      LOG(FATAL) << "Leads to resync process to start, not implemented.";
      break;
    case falcon::NackCode::kNotANack:
      LOG(FATAL) << "NACK handler invoked in case of a non-NACK packet.";
  }
  falcon_->get_rate_update_engine()->NackReceived(packet);
  // Timeout states may change, so reschedule.
  CHECK_OK(SetupRetransmitTimer(connection_state));
  return absl::OkStatus();
}

absl::Status ProtocolPacketReliabilityManager::HandleRnrNack(
    const Packet* rnr_nack) {
  // Get a handle on the connection state.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  uint32_t cid = rnr_nack->nack.dest_cid;
  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(cid));
  // Get the required metadata of the NACKed packet.
  uint32_t nacked_psn = rnr_nack->nack.nack_psn;
  auto* tx_window =
      GetAppropriateTxWindow(&connection_state->tx_reliability_metadata,
                             rnr_nack->nack.request_window);
  auto status = tx_window->GetOutstandingPacketRSN(nacked_psn);
  if (!status.ok()) {
    // Failed to get packet context of PSN being NACKed; one scenario where
    // this can happen is if the NACK has arrived after the ACK (e.g., due to
    // packet reordering when multipathing), in which case we can ignore the
    // NACK. Otherwise, this is an unexpected NACK and we should fail.
    if (nacked_psn < tx_window->base_packet_sequence_number) {
      return absl::OkStatus();
    }
    RETURN_IF_ERROR(status.status())
        << "Could not find packet context for NACKed PSN.";
  }
  auto packet_context = status.value();
  uint32_t nacked_rsn = packet_context->rsn;
  auto packet_type = packet_context->packet_type;
  // Set the packet metadata as RNR-NACKed and its timeout.
  auto packet = GetPacketMetadata(connection_state, nacked_rsn, packet_type);
  packet->nack_code = falcon::NackCode::kUlpReceiverNotReady;
  packet->ulp_nack_metadata.rnr_timeout = rnr_nack->nack.rnr_timeout;
  CHECK_OK(SetupRetransmitTimer(connection_state));
  return absl::OkStatus();
}

// Verifies if initial transmission meets Tx gating criteria in the following
// manner -
// 1. Pull Request / Push Request: PSN < RBPSN + fcwnd && Outstanding Request
// Count (ORC) < ncwnd
// 2. Push Unsolicited Data: PSN < DBPSN + fcwnd && ORC < ncwnd
// 3. Push Solicited Data / Pull Data / Push Grant: PSN < DBPSN + fcwnd
bool ProtocolPacketReliabilityManager::
    MeetsInitialTransmissionCCTxGatingCriteria(uint32_t scid,
                                               falcon::PacketType type) {
  // Get a handle on the connection state and reliability metadata.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       state_manager->PerformDirectLookup(scid));
  auto* tx_window =
      GetAppropriateTxWindow(&connection_state->tx_reliability_metadata, type);

  uint32_t open_fcwnd = GetOpenFcwnd(scid, type);
  if (open_fcwnd == 0) {
    return false;
  }
  if ((type == falcon::PacketType::kPullRequest) ||
      (type == falcon::PacketType::kPushRequest) ||
      (type == falcon::PacketType::kPushUnsolicitedData)) {
    // ORC < ncwnd
    if (tx_window->outstanding_requests_counter <
        connection_state->congestion_control_metadata->nic_congestion_window) {
      return true;
    } else {
      return false;
    }
  }
  return true;
}

uint32_t ProtocolPacketReliabilityManager::GetOpenFcwnd(
    uint32_t cid, falcon::PacketType type) {
  // Get a handle on the connection state and reliability metadata.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       state_manager->PerformDirectLookup(cid));
  auto* tx_window =
      GetAppropriateTxWindow(&connection_state->tx_reliability_metadata, type);
  double fcwnd = falcon_rue::FixedToFloat<uint32_t, double>(
      connection_state->congestion_control_metadata->fabric_congestion_window,
      falcon_rue::kFractionalBits);
  // Rounds down if fcwnd > 1, else rounds up.
  uint32_t rounded_fcwnd = 1;
  if (fcwnd > 1) {
    rounded_fcwnd = std::floor(fcwnd);
  }
  // FCWND should always be <= Window Size. If not, it points to a
  // configuration error in either CC max cwnd limit or window size.
  CHECK_LE(rounded_fcwnd, tx_window->window->Size());
  uint32_t psn_bound = tx_window->base_packet_sequence_number + rounded_fcwnd;
  // PSN >= PSN + fcwnd
  uint32_t future_psn = tx_window->next_packet_sequence_number;
  if (!SeqLT(future_psn, psn_bound)) {
    return 0;
  }
  return psn_bound - future_psn;
}

bool ProtocolPacketReliabilityManager::
    MeetsInitialTransmissionOowTxGatingCriteria(uint32_t scid,
                                                falcon::PacketType type) {
  if (enable_pause_initial_transmission_on_oow_drops_) {
    // Get a handle on the connection state and reliability metadata.
    ConnectionStateManager* const state_manager = falcon_->get_state_manager();
    CHECK_OK_THEN_ASSIGN(auto connection_state,
                         state_manager->PerformDirectLookup(scid));
    auto* tx_window = GetAppropriateTxWindow(
        &connection_state->tx_reliability_metadata, type);
    return !tx_window->oow_notification_received;
  } else {
    return true;
  }
}

// Verifies if retransmission meets Tx gating criteria  in the following
// manner:
// 1. Pull Request / Push Request: PSN < RBPSN + fcwnd
// 2. Push Unsolicited Data: PSN < DBPSN + fcwnd
// 3. Push Solicited Data / Pull Data / Push Grant: PSN < DBPSN + fcwnd
bool ProtocolPacketReliabilityManager::MeetsRetransmissionCCTxGatingCriteria(
    uint32_t scid, uint32_t psn, falcon::PacketType type) {
  // Get a handle on the connection state and reliability metadata.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       state_manager->PerformDirectLookup(scid));
  auto* tx_window =
      GetAppropriateTxWindow(&connection_state->tx_reliability_metadata, type);
  double fcwnd = falcon_rue::FixedToFloat<uint32_t, double>(
      connection_state->congestion_control_metadata->fabric_congestion_window,
      falcon_rue::kFractionalBits);
  // Rounds down if fcwnd > 1, else rounds up.
  uint32_t rounded_fcwnd = 1;
  if (fcwnd > 1) {
    rounded_fcwnd = std::floor(fcwnd);
  }
  // FCWND should always be <= Window Size. If not, it points to a
  // configuration error in either CC max cwnd limit or window size.
  CHECK_LE(rounded_fcwnd, tx_window->window->Size());
  uint32_t psn_bound = tx_window->base_packet_sequence_number + rounded_fcwnd;
  if (!SeqLT(psn, psn_bound)) {
    return false;
  }
  if ((type == falcon::PacketType::kPullRequest) ||
      (type == falcon::PacketType::kPushRequest) ||
      (type == falcon::PacketType::kPushUnsolicitedData)) {
    // ORRC < ncwnd
    if (tx_window->outstanding_retransmission_requests_counter <
        connection_state->congestion_control_metadata->nic_congestion_window) {
      return true;
    } else {
      return false;
    }
  }
  return true;
}

// Transmits ULP NACK over the network.
absl::Status ProtocolPacketReliabilityManager::HandleNackFromUlp(
    uint32_t scid, const TransactionKey& transaction_key,
    UlpNackMetadata* ulp_nack_metadata, const OpaqueCookie& cookie) {
  // Get a handle on the connection state along with receiver context.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(scid));
  ASSIGN_OR_RETURN(
      const ReceivedPacketContext* const recv_pkt_ctx,
      connection_state->rx_reliability_metadata.GetReceivedPacketContext(
          transaction_key));

  // Get a handle on the packet metadata.
  CHECK_OK_THEN_ASSIGN(auto transaction,
                       connection_state->GetTransaction(transaction_key));
  CHECK_OK_THEN_ASSIGN(auto packet_metadata,
                       transaction->GetPacketMetadata(recv_pkt_ctx->type));
  // Store the ULP NACK metadata.
  packet_metadata->nack_code =
      AckSyndromeToNackCode(ulp_nack_metadata->ulp_nack_code);
  packet_metadata->ulp_nack_metadata = *ulp_nack_metadata;

  // If this is RNR NACK to a PullReq, don't send NACK.
  if (ulp_nack_metadata->ulp_nack_code == Packet::Syndrome::kRnrNak &&
      recv_pkt_ctx->type == falcon::PacketType::kPullRequest)
    return absl::OkStatus();

  // Get a handle on the ack coalescing engine.
  AckCoalescingEngineInterface* ack_coalescing_engine =
      falcon_->get_ack_coalescing_engine();
  auto ack_coalescing_key =
      ack_coalescing_engine->GenerateAckCoalescingKeyFromUlp(scid, cookie);

  // Transmits the ULP NACK.
  return ack_coalescing_engine->TransmitNACK(
      *ack_coalescing_key, recv_pkt_ctx->psn,
      BelongsToRequestWindow(recv_pkt_ctx->type),
      AckSyndromeToNackCode(ulp_nack_metadata->ulp_nack_code),
      ulp_nack_metadata);
}

void ProtocolPacketReliabilityManager::DequeuePacketFromRetxScheduler(
    uint32_t scid, uint32_t rsn, falcon::PacketType type,
    TransactionMetadata* transaction) {
  CHECK_OK(
      falcon_->get_retransmission_scheduler()->DequeuePacket(scid, rsn, type));
}

// Sends an ACK (completion) to ULP in case it corresponds to a write op via
// the reorder engine.
void ProtocolPacketReliabilityManager::EnqueueAckToUlp(
    TransmitterReliabilityWindowMetadata* window, uint32_t scid, uint32_t psn) {
  CHECK_OK_THEN_ASSIGN(auto packet_context,
                       window->GetOutstandingPacketRSN(psn));
  uint32_t rsn = packet_context->rsn;
  auto type = packet_context->packet_type;
  // We only send an ACK/completion to ULP for outgoing request. As a results,
  // the corresponding packet is NOT an incoming one.
  auto location = GetTransactionLocation(/*type=*/type, /*incoming=*/false);
  // Get a handle on the connection state.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       state_manager->PerformDirectLookup(scid));
  // Update transaction state when an ack is received.
  CHECK_OK_THEN_ASSIGN(auto transaction,
                       connection_state->GetTransaction({rsn, location}));
  transaction->UpdateState(type, TransactionStage::NtwkAckRx, falcon_);
  // Decrement outstanding request count if applicable.
  DecrementOutstandingRequestCount(scid, type);
  // Decrement ORRC in case the request corresponding to ACK of Push Solicited
  // Data was retransmitted.
  if (type == falcon::PacketType::kPushSolicitedData) {
    CHECK_OK_THEN_ASSIGN(auto request, transaction->GetPacketMetadata(
                                           falcon::PacketType::kPushRequest));
    if (request->transmit_attempts > 1) {
      DecrementOutstandingRetransmittedRequestCount(scid, type, true);
    }
  }

  // Clean up outstanding packet context corresponding to ACKed packets.
  if (!window->outstanding_packets.erase(
          RetransmissionWorkId(rsn, psn, type))) {
    DequeuePacketFromRetxScheduler(scid, rsn, type, transaction);
  } else {
    CHECK_OK_THEN_ASSIGN(PacketMetadata* const packet_metadata,
                         transaction->GetPacketMetadata(type));
    if ((type == falcon::PacketType::kPullRequest ||
         type == falcon::PacketType::kPushUnsolicitedData) &&
        packet_metadata->transmit_attempts > 1) {
      DecrementOutstandingRetransmittedRequestCount(scid, type, true);
    }
  }
  // Increment num_acked by one in connection state. This is the only place
  // where num_acked in the connection state is incremented.
  AfterPacketAcked(connection_state, packet_context);
  window->outstanding_packet_contexts.erase(psn);

  if (type == falcon::PacketType::kPushSolicitedData ||
      type == falcon::PacketType::kPushUnsolicitedData) {
    CHECK_OK(falcon_->get_buffer_reorder_engine()->InsertPacket(
        falcon::PacketType::kAck, scid, rsn, 0));
  } else {
    // Release TX resources corresponding to transaction that was ACKed. TX
    // resources corresponding to push solicited/unsolicited transactions
    // released along to RX resources to minimize QP cache context bandwidth.
    // Note: While operating in the Gen_1 resource management mode, the resource
    // manager does not release resources corresponding to Push Solicited
    // Request as that is released along with RX resources when the completion
    // is delivered.
    CHECK_OK(falcon_->get_resource_manager()->ReleaseResources(
        scid, {rsn, location}, type));
  }

  // Delete metadata and receiver packet context corresponding to pull
  // transactions, if pull response is ACKed from initiator.
  if (type == falcon::PacketType::kPullData) {
    connection_state->transactions.erase({rsn, location});
    connection_state->rx_reliability_metadata.received_packet_contexts.erase(
        {rsn, location});
  }
}

// Handles ACK from the ULP (Pull Request, Push Solicited Data, Push
// Unsolicited Data) in the following manner -
// 1. Push Solicited or Push Unsolicited Data : update the receiver ack
// bitmap, update the ack state, delete the received packet context
// 2. Pull request: do nothing as the received packet context is required when
// we receive an ACK corresponding to the Pull data to return credits to RDMA;
// no no need to update the ack bitmap and the ack state as that is done
// immediately by FALCON when the request is received)
absl::Status ProtocolPacketReliabilityManager::HandleAckFromUlp(
    uint32_t scid, uint32_t rsn, const OpaqueCookie& cookie) {
  // Get a handle on the connection state.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(scid));
  // Get a handle on the receiver packet context.
  ASSIGN_OR_RETURN(
      const ReceivedPacketContext* const pkt_context,
      connection_state->rx_reliability_metadata.GetReceivedPacketContext(
          {rsn, TransactionLocation::kTarget}));
  falcon::PacketType type = pkt_context->type;
  uint32_t psn = pkt_context->psn;
  if (type == falcon::PacketType::kPullRequest) {
    return absl::OkStatus();
  }
  // Get a handle on the ack coalescing engine
  AckCoalescingEngineInterface* ack_coalescing_engine =
      falcon_->get_ack_coalescing_engine();
  auto ack_coalescing_key =
      ack_coalescing_engine->GenerateAckCoalescingKeyFromUlp(scid, cookie);
  // Check if we should send back an ACK due to AR-bit.
  bool ack_requested = pkt_context->is_ack_requested;
  // Delete the receiver packet context in case of push solicited/unsolicited
  // data.
  connection_state->rx_reliability_metadata.received_packet_contexts.erase(
      {rsn, TransactionLocation::kTarget});
  // Delete metadata related to the transaction in case of push
  // solicited/unsolicited data.
  connection_state->transactions.erase({rsn, TransactionLocation::kTarget});
  // For the push solicited/unsolicited data transactions, get the appropriate
  // RX window metadata.
  auto* rx_window_metadata =
      GetAppropriateRxWindow(&connection_state->rx_reliability_metadata, type);
  // Update the receiver ACK bitmap.
  UpdateReceiveAckBitmap(&connection_state->rx_reliability_metadata,
                         rx_window_metadata, psn);
  // Generate ACK or update coalescing state.
  RETURN_IF_ERROR(ack_coalescing_engine->GenerateAckOrUpdateCoalescingState(
      std::move(ack_coalescing_key), ack_requested, 1));
  return absl::OkStatus();
}

// Increments outstanding request count when sending out a request.
void ProtocolPacketReliabilityManager::IncrementOutstandingRequestCount(
    ConnectionState* const connection_state, falcon::PacketType type) {
  if (type == falcon::PacketType::kPullRequest ||
      type == falcon::PacketType::kPushRequest ||
      type == falcon::PacketType::kPushUnsolicitedData) {
    auto* tx_window = GetAppropriateTxWindow(
        &connection_state->tx_reliability_metadata, type);
    tx_window->outstanding_requests_counter++;
  }
}

// Decrements outstanding request count when an ACK is received.
void ProtocolPacketReliabilityManager::DecrementOutstandingRequestCount(
    uint32_t scid, falcon::PacketType type) {
  if (type == falcon::PacketType::kPullRequest ||
      type == falcon::PacketType::kPushSolicitedData ||
      type == falcon::PacketType::kPushUnsolicitedData) {
    // Get a handle on the connection state.
    ConnectionStateManager* const state_manager = falcon_->get_state_manager();
    CHECK_OK_THEN_ASSIGN(auto connection_state,
                         state_manager->PerformDirectLookup(scid));
    // Only PushUnsolicitedData should decrement the data-orc. Specifically, Ack
    // for *PushSolicitedData* should decrement req-orc, because PushRequest
    // increment the req-orc.
    auto* tx_window =
        type == falcon::PacketType::kPushUnsolicitedData
            ? &connection_state->tx_reliability_metadata.data_window_metadata
            : &connection_state->tx_reliability_metadata
                   .request_window_metadata;
    tx_window->outstanding_requests_counter--;
  }
}

// Increments outstanding retransmitted request count when sending out a
// retransmitted request.
void ProtocolPacketReliabilityManager::
    IncrementOutstandingRetransmittedRequestCount(
        ConnectionState* const connection_state, falcon::PacketType type) {
  if (type == falcon::PacketType::kPullRequest ||
      type == falcon::PacketType::kPushRequest ||
      type == falcon::PacketType::kPushUnsolicitedData) {
    auto* tx_window = GetAppropriateTxWindow(
        &connection_state->tx_reliability_metadata, type);
    tx_window->outstanding_retransmission_requests_counter++;
  }
}

// Decrements outstanding request count when -
// (a) ACK is received corresponding to a retransmitted request or
// (b) When >1 retx of a packet is enqueued in the retransmission scheduler.
// Note: In case of solicited writes, we should not decrement ORRC on receiving
// ACK corresponding to PushRequest. We decrement when we enqueue PushRequest in
// retransmission scheduler or receive ACK corresponding to PushSolicitedData.
void ProtocolPacketReliabilityManager::
    DecrementOutstandingRetransmittedRequestCount(uint32_t scid,
                                                  falcon::PacketType type,
                                                  bool is_acked) {
  if (type == falcon::PacketType::kPullRequest ||
      type == falcon::PacketType::kPushUnsolicitedData ||
      (type == falcon::PacketType::kPushSolicitedData && is_acked) ||
      (type == falcon::PacketType::kPushRequest && !is_acked)) {
    // Get a handle on the connection state.
    ConnectionStateManager* const state_manager = falcon_->get_state_manager();
    CHECK_OK_THEN_ASSIGN(auto connection_state,
                         state_manager->PerformDirectLookup(scid));
    // Only PushUnsolicitedData should decrement the data-orc. Specifically, Ack
    // for *PushSolicitedData* should decrement req-orc, because PushRequest
    // increment the req-orc.
    auto* tx_window =
        type == falcon::PacketType::kPushUnsolicitedData
            ? &connection_state->tx_reliability_metadata.data_window_metadata
            : &connection_state->tx_reliability_metadata
                   .request_window_metadata;
    tx_window->outstanding_retransmission_requests_counter--;
  }
}

// Determines if the AR bit set criteria is met or not. Returns true if fcwnd
// <= ar_threshold or with ar_percent probability.
bool ProtocolPacketReliabilityManager::MeetsAckRequestedBitSetCriteria(
    uint32_t fcwnd) {
  if (!falcon_->get_config()->enable_ack_request_bit()) return false;
  ++passed_packets_for_ar_;
  if (passed_packets_for_ar_ >= next_ar_) {
    passed_packets_for_ar_ = 0;
    next_ar_ =
        static_cast<uint8_t>(ar_bit_set_generator_(random_number_generator_));
    return true;
  }
  return fcwnd <= falcon_->get_config()->ack_request_fcwnd_threshold();
}

bool ProtocolPacketReliabilityManager::
    MeetsAckRequestedBitSetCriteriaForTesting(uint32_t fcwnd) {
  return MeetsAckRequestedBitSetCriteria(fcwnd);
}

// The OOO-count algorithm for early retransmission, which decides to retransmit
// the holes which have enough (>=ooo_threshold_) '1' after them.
std::vector<int> ProtocolPacketReliabilityManager::EarlyRetransmissionOooCount(
    const std::array<AckWindowMetadata, 2>& ack_received_metadata) {
  std::vector<int> retx_limit(2, 0);
  for (int window_index = 0; window_index < 2; window_index++) {
    const auto& ack_metadata = ack_received_metadata[window_index];
    // Find the upper limit of index in the bitmap that can be retransmitted due
    // to OOO-count. If there is insufficient OOO, retx_limit will be -1.
    int count = 0;
    int& limit = retx_limit[window_index];
    for (limit = ack_metadata.window.Size() - 1;
         limit >= 0 && count < ooo_count_threshold_; limit--) {
      count += ack_metadata.window.Get(limit);
    }
  }
  return retx_limit;
}

std::vector<int>
ProtocolPacketReliabilityManager::EarlyRetransmissionOooDistance(
    const std::array<AckWindowMetadata, 2>& ack_received_metadata) {
  std::vector<int> retx_limit(2, 0);
  for (int window_index = 0; window_index < 2; window_index++) {
    auto& ack_metadata = ack_received_metadata[window_index];
    int& limit = retx_limit[window_index];
    // Find the highest index that is received.
    for (limit = ack_metadata.window.Size() - 1; limit >= 0; limit--)
      if (ack_metadata.window.Get(limit)) break;
    limit -= ooo_distance_threshold_;
  }
  return retx_limit;
}

void ProtocolPacketReliabilityManager::EarlyRetransmissionMergeRetxLimit(
    std::vector<int>& retx_limit, const std::vector<int>& new_limit) {
  // For each window (request & data), update to the larger of the limits.
  for (int i = 0; i < 2; i++)
    if (retx_limit[i] < new_limit[i]) {
      retx_limit[i] = new_limit[i];
    }
}

bool ProtocolPacketReliabilityManager::EackBasedEarlyRetransmissionIsEnabled()
    const {
  return enable_ooo_distance_ || enable_ooo_count_;
}

uint32_t ProtocolPacketReliabilityManager::ChooseOutgoingPacketFlowLabel(
    falcon::PacketType packet_type, uint32_t rsn,
    ConnectionState* connection_state) {
  return connection_state->congestion_control_metadata->flow_label;
}

// Post-process (update the related counters, etc.) after flow label is
// chosen.
void ProtocolPacketReliabilityManager::AfterChooseOutgoingPacketFlowLabel(
    falcon::PacketType packet_type, uint32_t rsn,
    ConnectionState* connection_state, uint32_t old_flow_label,
    uint32_t new_flow_label) {}

}  // namespace isekai
