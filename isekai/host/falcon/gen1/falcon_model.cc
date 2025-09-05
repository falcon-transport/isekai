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

#include "isekai/host/falcon/gen1/falcon_model.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_factories.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"

namespace isekai {

FalconModel::FalconModel(const FalconConfig& configuration, Environment* env,
                         StatisticCollectionInterface* stats_collector,
                         ConnectionManagerInterface* connection_manager,
                         std::string_view host_id,
                         const uint8_t number_of_hosts)
    : configuration_(configuration),
      env_(env),
      stats_collector_(stats_collector),
      host_id_(host_id),
      connection_manager_(connection_manager),
      stats_manager_(CreateStatsManager(this)),
      conn_state_manager_(CreateConnectionStateManager(this)),
      resource_manager_(CreateResourceManager(this)),
      inter_host_rx_scheduler_(
          CreateInterHostRxScheduler(this, number_of_hosts)),
      connection_scheduler_(CreateConnectionScheduler(this)),
      retransmission_scheduler_(CreateRetransmissionScheduler(this)),
      ack_nack_scheduler_(CreateAckNackScheduler(this)),
      arbiter_(CreateArbiter(this)),
      admission_control_manager_(CreateAdmissionControlManager(this)),
      packet_reliability_manager_(CreatePacketReliabilityManager(this)),
      rate_update_engine_(CreateRateUpdateEngine(this)),
      buffer_reorder_engine_(CreateBufferReorderEngine(this)),
      ack_coalescing_engine_(CreateAckCoalescingEngine(this)),
      packet_metadata_transformer_(CreatePacketMetadataTransformer(this)) {}

// Initiates FALCON transactions per the RDMA Op received in the following
// manner -
// 1. Initialize FALCON transaction metadata corresponding to the RDMA Op.
// 2. Reserve resources for the FALCON transaction using the flow control
// manager. If no resources are available, initiate backpressure to RDMA block.
// 3. Finally, enqueue the FALCON transaction to the connection scheduler so
// that it can be picked up by the packet reliability manager.
void FalconModel::InitiateTransaction(std::unique_ptr<Packet> packet) {
  VLOG(2) << "[" << get_host_id() << ": " << env_->ElapsedTime() << "]["
          << packet->metadata.scid << ", -, -] " << "Receives packet from ULP: "
          << static_cast<int>(packet->rdma.opcode)
          << " with request length: " << packet->rdma.request_length;
  // Update ULP Rx counters on receiving a transaction from RDMA.
  stats_manager_->UpdateUlpRxCounters(packet->rdma.opcode,
                                      packet->metadata.scid);
  // Get a handle on the FALCON connection state via the connection state
  // manager.
  uint32_t scid = packet->metadata.scid;
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       conn_state_manager_->PerformDirectLookup(scid));
  // Reserve resources corresponding to the received RDMA op. If fails, exert
  // backpressure to RDMA block.
  if (!resource_manager_
           ->VerifyResourceAvailabilityOrReserveResources(
               scid, packet.get(), PacketDirection::kOutgoing, true)
           .ok()) {
    LOG(FATAL)
        << "FALCON received a packet for which it does not have resources; "
           "FALCON to RDMA XoFF thresholds not set appropriately as FALCON "
           "should have signalled an XoFF earlier.";
  }
  // Create or update transaction metadata corresponding to the received RDMA
  // op as it passes the resource checks.
  auto ulp_trans_info = connection_state->CreateOrUpdateTransactionFromUlp(
      std::move(packet), configuration_.threshold_solicit(),
      connection_state->connection_metadata->ordered_mode, this);

  stats_manager_->UpdateRxFromUlpRsnSeries(scid, ulp_trans_info.rsn,
                                           ulp_trans_info.type);
  // Enqueue transaction to connection scheduler. If fails, exert backpressure
  // to RDMA block.
  if (!connection_scheduler_
           ->EnqueuePacket(scid, ulp_trans_info.rsn, ulp_trans_info.type)
           .ok()) {
    LOG(FATAL) << "Enqueuing to FALCON connection failed unexpectedly.";
  }
}

// Handles the initial checks corresponding to the incoming packet  -
// 1. For transactions - reserve resources, check request/response lengths and
// hand it off to the packet reliability manager (which performs the sliding
// window checks and interacts with the reorder engine).
// 2. For ACKs/NACKs - hand it off to the packet reliablity manager for updating
// sliding window state.
void FalconModel::TransferRxPacket(std::unique_ptr<Packet> packet) {
  VLOG(2) << "[" << get_host_id() << ": " << env_->ElapsedTime() << "]["
          << packet->falcon.dest_cid << ", " << packet->falcon.rsn << ", "
          << static_cast<int>(packet->packet_type) << "] "
          << "Receives packet from network.";
  //
  // field across all packet types.
  uint32_t cid = packet->falcon.dest_cid;
  // But if packet type if ACK or NACK, then dest_cid is in the other header.
  if (packet->packet_type == falcon::PacketType::kAck) {
    cid = packet->ack.dest_cid;
  } else if (packet->packet_type == falcon::PacketType::kNack) {
    cid = packet->nack.dest_cid;
  }
  stats_manager_->UpdateNetworkRxCounters(packet->packet_type, cid);
  auto* pkt_ptr = packet.get();
  if (IsFalconTransaction(packet->packet_type)) {
    // Packets from the network have PSN and RSN assigned, so do not modify
    // those fields.
    uint32_t psn = packet->falcon.psn;
    falcon::PacketType type = packet->packet_type;
    // Get a handle on the FALCON connection state via the connection state
    // manager.
    CHECK_OK_THEN_ASSIGN(auto connection_state,
                         conn_state_manager_->PerformDirectLookup(cid));

    // Do the sliding window checks and ACK processing.
    auto status = packet_reliability_manager_->ReceivePacket(pkt_ptr);
    if (!status.ok()) {
      stats_manager_->UpdateNetworkRxDropCounters(packet->packet_type, cid,
                                                  status);
      if (status.code() == absl::StatusCode::kResourceExhausted) {
        // Generates NACK to indicate RX resource exhaustion.
        auto ack_coalescing_key =
            ack_coalescing_engine_->GenerateAckCoalescingKeyFromIncomingPacket(
                pkt_ptr);
        CHECK_OK(ack_coalescing_engine_->TransmitNACK(
            *ack_coalescing_key, psn, BelongsToRequestWindow(type),
            falcon::NackCode::kRxResourceExhaustion, nullptr));
      }
      return;
    }

    // Create or update transaction metadata based on the received transaction.
    auto network_trans_info =
        connection_state->CreateOrUpdateTransactionFromNetwork(
            std::move(packet), this);
    // Verify that the request/response length is per the original request.
    if ((type == falcon::PacketType::kPullData ||
         type == falcon::PacketType::kPushGrant ||
         type == falcon::PacketType::kPushSolicitedData) &&
        network_trans_info.expected_length !=
            network_trans_info.actual_length) {
      LOG(FATAL)
          << "Request/response length of packet differs from original request.";
    }

    stats_manager_->UpdateNetworkAcceptedRsnSeries(cid, pkt_ptr->falcon.rsn,
                                                   pkt_ptr->packet_type);

    // Reserve resources for the incoming FALCON transaction. This should always
    // succeed as resource reservation errors are caught within the sliding
    // window checks.
    CHECK_OK(resource_manager_->VerifyResourceAvailabilityOrReserveResources(
        cid, pkt_ptr, PacketDirection::kIncoming, true));
    if (pkt_ptr->packet_type != falcon::PacketType::kResync) {
      // Enqueue the packet in the reorder engine.
      auto status = buffer_reorder_engine_->InsertPacket(
          pkt_ptr->packet_type, cid, pkt_ptr->falcon.rsn, pkt_ptr->falcon.ssn);
      // If the connection is in RNR state, inform reliability manager which
      // will send RNR NACK to initiator.
      if (status.code() == absl::StatusCode::kFailedPrecondition) {
        CHECK_OK_THEN_ASSIGN(
            auto transaction,
            connection_state->GetTransaction(
                {pkt_ptr->falcon.rsn, TransactionLocation::kTarget}));
        transaction->UpdateState(pkt_ptr->packet_type,
                                 TransactionStage::UlpRnrRx, this);
        auto rnr_timeout = buffer_reorder_engine_->GetRnrTimeout(cid);
        UlpNackMetadata nack_metadata = UlpNackMetadata();
        nack_metadata.rnr_timeout = rnr_timeout;
        nack_metadata.ulp_nack_code = Packet::Syndrome::kRnrNak;
        std::unique_ptr<OpaqueCookie> cookie = CreateCookie(*pkt_ptr);
        CHECK_OK(packet_reliability_manager_->HandleNackFromUlp(
            cid, {pkt_ptr->falcon.rsn, TransactionLocation::kTarget},
            &nack_metadata, *cookie));
      } else {
        CHECK_OK(status);
      }
    }
  } else {
    auto status = packet_reliability_manager_->ReceivePacket(pkt_ptr);
    if (!status.ok()) {
      stats_manager_->UpdateNetworkRxDropCounters(packet->packet_type, cid,
                                                  status);
    }
  }
}

// Handles the (N)ACKs received from RDMA -
// 1. ACKs: Releases RX resources corresponding to the transactions. Updates the
// transaction metadata and informs the sliding window layer of the received
// push data ACKs. Refund admission control reserved bytes, if ACK corresponds
// to push solicited transaction.
// 2. NACKs: Generate the corresponding ULP NACK and transmit via the sliding
// window layer.
void FalconModel::AckTransaction(uint32_t scid, uint32_t rsn,
                                 Packet::Syndrome ack_code,
                                 absl::Duration rnr_timeout,
                                 std::unique_ptr<OpaqueCookie> cookie) {
  VLOG(2) << "[" << get_host_id() << ": " << env_->ElapsedTime() << "][" << scid
          << ", " << rsn << ", " << static_cast<int>(ack_code) << "] "
          << "Receives ACK from ULP.";
  // Update ACK/NACK counters on receiving acks from RDMA.
  if (ack_code == Packet::Syndrome::kAck) {
    stats_manager_->UpdateUlpRxCounters(Packet::Rdma::Opcode::kAck, scid);
  } else {
    stats_manager_->UpdateUlpRxCounters(Packet::Rdma::Opcode::kInvalid, scid);
  }
  // Get a handle on the FALCON connection state via the state manager.
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       conn_state_manager_->PerformDirectLookup(scid));
  if (ack_code == Packet::Syndrome::kAck) {
    // ACK: Get a handle on the transaction and update the state. Only ULP at
    // the target generate ACKs to Falcon.
    CHECK_OK_THEN_ASSIGN(
        auto transaction,
        connection_state->GetTransaction({rsn, TransactionLocation::kTarget}));
    // Ensure that ACKs are generated only by the target host.
    CHECK(transaction->location == TransactionLocation::kTarget)
        << "Initiator cannot generate ACKs." << rsn;
    // Update the transaction state based on transaction type.
    switch (transaction->type) {
      case TransactionType::kPull:
        transaction->UpdateState(falcon::PacketType::kPullRequest,
                                 TransactionStage::UlpAckRx, this);
        // Release resources corresponding to the Pull Request (RX resources).
        CHECK_OK(resource_manager_->ReleaseResources(
            scid, {rsn, TransactionLocation::kTarget},
            falcon::PacketType::kPullRequest));
        break;
      case TransactionType::kPushSolicited:
        transaction->UpdateState(falcon::PacketType::kPushSolicitedData,
                                 TransactionStage::UlpAckRx, this);
        CHECK_OK(admission_control_manager_->RefundAdmissionControlResource(
            scid, {rsn, TransactionLocation::kTarget}));
        // Release resources corresponding to the Push Solicited Data (RX
        // resources). TX resources (corresponding to Push Grant) released when
        // we receive an ACK corresponding to it.
        CHECK_OK(resource_manager_->ReleaseResources(
            scid, {rsn, TransactionLocation::kTarget},
            falcon::PacketType::kPushSolicitedData));
        break;
      case TransactionType::kPushUnsolicited:
        transaction->UpdateState(falcon::PacketType::kPushUnsolicitedData,
                                 TransactionStage::UlpAckRx, this);
        // Release resources corresponding to the Push Unsolicited Data (RX
        // resources).
        CHECK_OK(resource_manager_->ReleaseResources(
            scid, {rsn, TransactionLocation::kTarget},
            falcon::PacketType::kPushUnsolicitedData));
        break;
    }
    CHECK_OK(packet_reliability_manager_->HandleAckFromUlp(scid, rsn, *cookie));
    // Reorder engine needs to keep states for RSNs that can be RNR-NACKed.
    // These states needs to be released when getting ULP-ACK.
    buffer_reorder_engine_->HandleAckFromUlp(scid, rsn);
  } else {
    UlpNackMetadata nack_metadata = UlpNackMetadata();
    nack_metadata.rnr_timeout = rnr_timeout;
    nack_metadata.ulp_nack_code = ack_code;
    CHECK_OK(packet_reliability_manager_->HandleNackFromUlp(
        scid, {rsn, TransactionLocation::kTarget}, &nack_metadata, *cookie));
    if (ack_code == Packet::Syndrome::kRnrNak) {
      CHECK_OK_THEN_ASSIGN(auto transaction,
                           connection_state->GetTransaction(
                               {rsn, TransactionLocation::kTarget}));
      switch (transaction->type) {
        case TransactionType::kPull:
          transaction->UpdateState(falcon::PacketType::kPullRequest,
                                   TransactionStage::UlpRnrRx, this);
          break;
        case TransactionType::kPushSolicited:
          transaction->UpdateState(falcon::PacketType::kPushSolicitedData,
                                   TransactionStage::UlpRnrRx, this);
          break;
        case TransactionType::kPushUnsolicited:
          transaction->UpdateState(falcon::PacketType::kPushUnsolicitedData,
                                   TransactionStage::UlpRnrRx, this);
          break;
      }

      buffer_reorder_engine_->HandleRnrNackFromUlp(scid, rsn, rnr_timeout);
    }
  }
}

// This is called by RDMA when a new QP is set up. If qp_type is RC, This
// function will store the QpId into the scid connection's metadata.
uint32_t FalconModel::SetupNewQp(uint32_t scid, QpId qp_id, QpType qp_type,
                                 OrderingMode ordering_mode) {
  if (qp_type == QpType::kUD) {
    LOG(FATAL) << "RDMA UD QP mode is not supported.";
  }
  return 0;
}

// Handles the remaining processing of a received packet once it is in order -
// 1. Forwards the packet to RDMA if it does not correspond to push request or
// grant.
// 2. In case of push request or grant, generate the subsequent transaction by
// reserving resources and enqueing it to the connection scheduler.
void FalconModel::ReorderCallback(uint32_t cid, uint32_t rsn,
                                  falcon::PacketType type) {
  // Get a handle on the FALCON connection state via the state manager.
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       conn_state_manager_->PerformDirectLookup(cid));
  const bool is_incoming_packet = true;
  switch (type) {
    case falcon::PacketType::kPullRequest:
    case falcon::PacketType::kPullData:
    case falcon::PacketType::kPushSolicitedData:
    case falcon::PacketType::kPushUnsolicitedData: {
      inter_host_rx_scheduler_->Enqueue(
          connection_state->connection_metadata->source_bifurcation_id,
          [&, connection_state, cid, rsn, type]() {
            TransactionLocation location =
                GetTransactionLocation(type, is_incoming_packet);
            CHECK_OK_THEN_ASSIGN(
                auto transaction,
                connection_state->GetTransaction({rsn, location}));
            CHECK_OK_THEN_ASSIGN(auto packet_metadata,
                                 transaction->GetPacketMetadata(type));
            transaction->UpdateState(type, TransactionStage::UlpTx, this);
            VLOG(2) << "[" << get_host_id() << ": " << env_->ElapsedTime()
                    << "][" << cid << ", " << rsn << ", "
                    << static_cast<int>(type) << "] "
                    << "Sending transaction to ULP.";
            // Update ULP Tx counters when sending transactions to RDMA.
            stats_manager_->UpdateUlpTxCounters(
                packet_metadata->active_packet->rdma.opcode, cid);
            // Update the inter-packet gap of the Rx scheduler based on the
            // payload length of the packet.
            inter_host_rx_scheduler_->UpdateInterPacketGap(
                packet_metadata->active_packet->falcon.payload_length);
            // Make a copy of the packet, so that if RNR-NACKed, we can retry.
            auto packet_to_ulp =
                std::make_unique<Packet>(*packet_metadata->active_packet);
            std::unique_ptr<OpaqueCookie> cookie = CreateCookie(*packet_to_ulp);
            rdma_->HandleRxTransaction(std::move(packet_to_ulp),
                                       std::move(cookie));
            // Release resources corresponding to Pull transaction on receiving
            // Pull Data and delete the corresponding metadata as well as
            // receiver packet context as receiving pull data implies
            // completion.
            if (type == falcon::PacketType::kPullData) {
              CHECK_OK(
                  admission_control_manager_->RefundAdmissionControlResource(
                      cid, {rsn, location}));
              // Release resources corresponding to the Pull transaction. FALCON
              // releases the RX resources corresponding to Pull data.
              CHECK_OK(resource_manager_->ReleaseResources(
                  cid, {rsn, location}, falcon::PacketType::kPullData));
              connection_state->rx_reliability_metadata.received_packet_contexts
                  .erase({rsn, location});
              connection_state->transactions.erase({rsn, location});
            }
          });
      break;
    }
    case falcon::PacketType::kPushRequest:
    case falcon::PacketType::kPushGrant: {
      // Reserve resources and schedule transmission of push grant to the
      // initiator.
      auto next_packet_type = GetNextFalconPacket(type);
      VLOG(2) << "[" << get_host_id() << ": " << env_->ElapsedTime() << "]["
              << cid << ", " << rsn << ", " << static_cast<int>(type) << "] "
              << "Initiating sending out of packet with type: "
              << static_cast<int>(next_packet_type);
      TransactionLocation location =
          GetTransactionLocation(type, is_incoming_packet);
      CHECK_OK_THEN_ASSIGN(auto transaction,
                           connection_state->GetTransaction({rsn, location}));
      CHECK_OK_THEN_ASSIGN(auto packet_metadata,
                           transaction->GetPacketMetadata(next_packet_type));

      CHECK_OK(
          connection_scheduler_->EnqueuePacket(cid, rsn, next_packet_type));
    } break;
    case falcon::PacketType::kAck: {
      inter_host_rx_scheduler_->Enqueue(
          connection_state->connection_metadata->source_bifurcation_id,
          [&, connection_state, cid, rsn, type]() {
            // Get the corresponding QP_ID which is a part of the
            // receiver packet context and then delete the
            // context.
            VLOG(2) << "[" << get_host_id() << ": " << env_->ElapsedTime()
                    << "][" << cid << ", " << rsn << ", "
                    << static_cast<int>(type) << "] " << "Sends ACK to ULP.";
            // ACK/completion is sent to ULP only at the
            // initiator.
            const auto location = TransactionLocation::kInitiator;
            CHECK_OK_THEN_ASSIGN(
                auto pkt_ctx, connection_state->rx_reliability_metadata
                                  .GetReceivedPacketContext({rsn, location}));
            stats_manager_->UpdateUlpTxCounters(Packet::Rdma::Opcode::kAck,
                                                cid);
            // Send completion to ULP.
            rdma_->HandleCompletion(
                pkt_ctx->qp_id, rsn, Packet::Syndrome::kAck,
                connection_state->connection_metadata->source_bifurcation_id);
            CHECK_OK_THEN_ASSIGN(
                auto transaction,
                connection_state->GetTransaction({rsn, location}));
            if (transaction->type == TransactionType::kPushSolicited) {
              CHECK_OK_THEN_ASSIGN(
                  auto transaction,
                  connection_state->GetTransaction({rsn, location}));
              transaction->UpdateState(falcon::PacketType::kPushSolicitedData,
                                       TransactionStage::UlpCompletionTx, this);
              // Release resources corresponding to the Push
              // solicited transaction. FALCON releases both TX
              // and RX resources together to minimize QP context
              // cache bandwidth.
              CHECK_OK(resource_manager_->ReleaseResources(
                  cid, {rsn, location},
                  falcon::PacketType::kPushSolicitedData));
            } else if (transaction->type == TransactionType::kPushUnsolicited) {
              CHECK_OK_THEN_ASSIGN(
                  auto transaction,
                  connection_state->GetTransaction({rsn, location}));
              transaction->UpdateState(falcon::PacketType::kPushUnsolicitedData,
                                       TransactionStage::UlpCompletionTx, this);
              // Release resources corresponding to the Push
              // solicited transaction. FALCON releases both TX
              // and RX resources together to minimize QP context
              // cache bandwidth.
              CHECK_OK(resource_manager_->ReleaseResources(
                  cid, {rsn, location},
                  falcon::PacketType::kPushUnsolicitedData));
            } else {
              LOG(FATAL) << "Cannot send completion to ULP for "
                            "non-push transaction.";
            }
            // Delete metadata corresponding to transaction.
            connection_state->rx_reliability_metadata.received_packet_contexts
                .erase({rsn, location});
            connection_state->transactions.erase({rsn, location});
          });
      break;
    }
    case falcon::PacketType::kNack:
      stats_manager_->UpdateUlpTxCounters(Packet::Rdma::Opcode::kAck, cid);
      //
      // RDMA block.
      break;
    case falcon::PacketType::kBack:
    case falcon::PacketType::kEack:
      //
      LOG(FATAL) << "Isekai doesn't use EACK/BACK packet type yet.";
      break;
    case falcon::PacketType::kResync:
      LOG(FATAL) << "Handling of such packets not implemented.";
    case falcon::PacketType::kInvalid:
      LOG(FATAL) << "Reorder callback received an invalid "
                    "packet.";
  }
}

absl::Status FalconModel::EstablishConnection(
    uint32_t scid, uint32_t dcid, uint8_t source_bifurcation_id,
    uint8_t destination_bifurcation_id, absl::string_view dst_ip_address,
    OrderingMode ordering_mode,
    const FalconConnectionOptions& connection_options) {
  // Connects scid to the (dcid, dst_ip_address). Works only for RC (assumes
  // that the local_cid is unique).
  auto connection_metadata = CreateConnectionMetadata(
      scid, dcid, source_bifurcation_id, destination_bifurcation_id,
      dst_ip_address, ordering_mode, connection_options);
  return conn_state_manager_->InitializeConnectionState(
      std::move(connection_metadata));
}

std::unique_ptr<ConnectionMetadata> FalconModel::CreateConnectionMetadata(
    uint32_t scid, uint32_t dcid, uint8_t source_bifurcation_id,
    uint8_t destination_bifurcation_id, absl::string_view dst_ip_address,
    OrderingMode ordering_mode,
    const FalconConnectionOptions& connection_options) {
  auto connection_metadata = std::make_unique<ConnectionMetadata>();
  FillConnectionMetadata(connection_metadata.get(), scid, dcid,
                         source_bifurcation_id, destination_bifurcation_id,
                         dst_ip_address, ordering_mode, connection_options);
  return connection_metadata;
}

void FalconModel::FillConnectionMetadata(
    ConnectionMetadata* connection_metadata, uint32_t& scid, uint32_t& dcid,
    uint8_t& source_bifurcation_id, uint8_t& destination_bifurcation_id,
    absl::string_view& dst_ip_address, OrderingMode& ordering_mode,
    const FalconConnectionOptions& connection_options) {
  connection_metadata->scid = scid;
  connection_metadata->dcid = dcid;
  connection_metadata->source_bifurcation_id = source_bifurcation_id;
  connection_metadata->destination_bifurcation_id = destination_bifurcation_id;
  connection_metadata->dst_ip = dst_ip_address;
  connection_metadata->ordered_mode = ordering_mode;
  // Set ACK coalescing thresholds on a per-connection basis.
  connection_metadata->ack_coalescing_threshold =
      configuration_.ack_coalescing_thresholds().count();
  connection_metadata->ack_coalescing_timeout = absl::Nanoseconds(
      configuration_.ack_coalescing_thresholds().timeout_ns());
  connection_metadata->static_routing_port_lists =
      connection_options.static_routing_port_lists;
}

void FalconModel::SetXoffByPacketBuilder(bool xoff) {
  packet_builder_xoff_ = xoff;
  if (!packet_builder_xoff_) {
    // If Xon is triggered, resumes the scheduled work if any.
    arbiter_->ScheduleSchedulerArbiter();
  }
}

void FalconModel::SetXoffByRdma(uint8_t bifurcation_id, bool xoff) {
  inter_host_rx_scheduler_->SetXoff(bifurcation_id, xoff);
}

void FalconModel::UpdateRxBytes(std::unique_ptr<Packet> packet,
                                uint32_t pkt_size_bytes) {
  uint32_t cid = GetFalconPacketConnectionId(*packet);
  stats_manager_->UpdatePacketBuilderRxBytes(cid, pkt_size_bytes);
}

void FalconModel::UpdateTxBytes(std::unique_ptr<Packet> packet,
                                uint32_t pkt_size_bytes) {
  uint32_t cid = GetFalconPacketConnectionId(*packet);
  stats_manager_->UpdatePacketBuilderTxBytes(cid, pkt_size_bytes);
}

std::unique_ptr<OpaqueCookie> FalconModel::CreateCookieForTesting(
    const Packet& packet) {
  return CreateCookie(packet);
}

std::unique_ptr<OpaqueCookie> FalconModel::CreateCookie(const Packet& packet) {
  return std::make_unique<OpaqueCookie>();
}

}  // namespace isekai
