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

#include "isekai/host/falcon/gen1/resource_manager.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <list>
#include <type_traits>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "glog/logging.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_resource_credits.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/falcon/rue/fixed.h"

namespace isekai {
namespace {
// Flag: enable_resource_manager_ema_occupancy
constexpr std::string_view kStatsResourceManagerRxBufferEma =
    "falcon.resources.rx_buffer_ema";
constexpr std::string_view kStatsResourceManagerRxPacketEma =
    "falcon.resources.rx_packet_ema";
constexpr std::string_view kStatsResourceManagerTxPacketEma =
    "falcon.resources.tx_packet_ema";
constexpr std::array<std::string_view, 3> kStatVectorResourceManagerEma = {
    kStatsResourceManagerRxBufferEma, kStatsResourceManagerRxPacketEma,
    kStatsResourceManagerTxPacketEma};
constexpr std::string_view
    kStatVectorResourceManagerNetworkRegionQuantizedOccupancy =
        "falcon.resources.network_region_quantized_occupancy";

// Flag: enable_global_resource_credits_timeline
constexpr std::string_view kStatsTxPktCreditsUlpReqs =
    "falcon.resources.tx_packet_credits.ulp_requests";
constexpr std::string_view kStatsTxPktCreditsUlpData =
    "falcon.resources.tx_packet_credits.ulp_data";
constexpr std::string_view kStatsTxPktCreditsNtwkReqs =
    "falcon.resources.tx_packet_credits.network_requests";
constexpr std::string_view kStatsTxBufferCreditsUlpReqs =
    "falcon.resources.tx_buffer_credits.ulp_requests";
constexpr std::string_view kStatsTxBufferCreditsUlpData =
    "falcon.resources.tx_buffer_credits.ulp_data";
constexpr std::string_view kStatsTxBufferCreditsNtwkReqs =
    "falcon.resources.tx_buffer_credits.network_requests";
constexpr std::string_view kStatsRxPktCreditsUlpReqs =
    "falcon.resources.rx_packet_credits.ulp_requests";
constexpr std::string_view kStatsRxPktCreditsNtwkReqs =
    "falcon.resources.rx_packet_credits.network_requests";
constexpr std::string_view kStatsRxBufferCreditsUlpReqs =
    "falcon.resources.rx_buffer_credits.ulp_requests";
constexpr std::string_view kStatsRxBufferCreditsNtwkReqs =
    "falcon.resources.rx_buffer_credits.network_requests";
}  // namespace

ProtocolResourceManager::ProtocolResourceManager(FalconModelInterface* falcon)
    : falcon_(falcon),
      global_credits_(FalconResourceCredits::Create(
          falcon_->get_config()->resource_credits())),
      request_xoff_limit_({
          .tx_packet_credits =
              {
                  .ulp_requests = falcon_->get_config()
                                      ->ulp_xoff_thresholds()
                                      .tx_packet_request(),
                  .ulp_data = 0,
                  .network_requests = 0,
              },
          .tx_buffer_credits =
              {
                  .ulp_requests = falcon_->get_config()
                                      ->ulp_xoff_thresholds()
                                      .tx_buffer_request(),
                  .ulp_data = 0,
                  .network_requests = 0,
              },
          .rx_packet_credits =
              {
                  .ulp_requests = falcon_->get_config()
                                      ->ulp_xoff_thresholds()
                                      .rx_packet_request(),
                  .network_requests = 0,
              },
          .rx_buffer_credits =
              {
                  .ulp_requests = falcon_->get_config()
                                      ->ulp_xoff_thresholds()
                                      .rx_buffer_request(),
                  .network_requests = 0,
              },
      }),
      global_xoff_limit_({
          .tx_packet_credits =
              {
                  .ulp_requests = 0,
                  .ulp_data = falcon_->get_config()
                                  ->ulp_xoff_thresholds()
                                  .tx_packet_data(),
                  .network_requests = 0,
              },
          .tx_buffer_credits =
              {
                  .ulp_requests = 0,
                  .ulp_data = falcon_->get_config()
                                  ->ulp_xoff_thresholds()
                                  .tx_buffer_data(),
                  .network_requests = 0,
              },
          .rx_packet_credits =
              {
                  .ulp_requests = 0,
                  .network_requests = 0,
              },
          .rx_buffer_credits =
              {
                  .ulp_requests = 0,
                  .network_requests = 0,
              },
      }),
      network_region_occupancy_(0) {
  collect_ema_occupancy_ = falcon_->get_stats_manager()
                               ->GetStatsConfig()
                               .enable_resource_manager_ema_occupancy();
  collect_resource_credit_timeline_ =
      falcon_->get_stats_manager()
          ->GetStatsConfig()
          .enable_global_resource_credits_timeline();
}

// Reserves the necessary TX/RX resources for the transaction, if required.
absl::Status
ProtocolResourceManager::VerifyResourceAvailabilityOrReserveResources(
    uint32_t scid, const Packet* packet, PacketDirection direction,
    bool reserve_resources) {
  // Get a handle on the FALCON configuration.
  const FalconConfig* const config = falcon_->get_config();
  auto reservation_mode = config->resource_reservation_mode();

  // Return without doing credit reservation in this mode.
  if (reservation_mode == FalconConfig::BYPASS_RESERVATION) {
    return absl::OkStatus();
  }

  // Get a handle on the connection state.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(scid));

  // Determine the FALCON transaction represented by this packet.
  falcon::PacketType type;
  switch (direction) {
    case PacketDirection::kOutgoing:
      if (packet->packet_type == falcon::PacketType::kInvalid) {
        // Represents a transaction received from ULP (RSN not yet assigned).
        type = RdmaOpcodeToPacketType(
            packet, falcon_->get_config()->threshold_solicit());
      } else {
        // Represents a FALCON initiated packet.
        type = packet->packet_type;
        CHECK(type == falcon::PacketType::kPushGrant ||
              type == falcon::PacketType::kPushSolicitedData);
      }
      break;
    case PacketDirection::kIncoming:
      // All incoming packets should have the packet_type set.
      type = packet->packet_type;
      break;
  }

  if (packet->packet_type == falcon::PacketType::kInvalid) {
    // Represents packet received from ULP (thus packet_type is not set and RSN
    // is not yet assigned).
    VLOG(2) << "[" << falcon_->get_host_id() << ": "
            << falcon_->get_environment()->ElapsedTime() << "][" << scid << ", "
            << ", " << ", " << static_cast<int>(type) << "] "
            << "Reserving resources for the transaction received from ULP.";
  } else {
    VLOG(2) << "[" << falcon_->get_host_id() << ": "
            << falcon_->get_environment()->ElapsedTime() << "][" << scid << ", "
            << packet->falcon.rsn << ", " << static_cast<int>(type) << "] "
            << "Reserving resources for the packet received from network "
               "or initiated by FALCON.";
  }

  FalconResourceCredits request_credits;
  switch (type) {
    case falcon::PacketType::kPullRequest:
      switch (direction) {
        case PacketDirection::kOutgoing:
          if (config->resource_reservation_mode() ==
              FalconConfig::FIRST_PHASE_RESERVATION) {
            // Reserves resources for both the pull request and its subsequent
            // pull data response.
            request_credits = ComputeOutgoingPullRequestResources(
                packet->rdma.data_length, packet->metadata.sgl_length);
            // Uses the expected response length of the Pull Response.
            request_credits +=
                ComputeIncomingPullDataResources(packet->rdma.request_length);
          }
          break;
        case PacketDirection::kIncoming:
          if (!MeetsNetworkRequestAdmissionCriteria(
                  packet->falcon.dest_cid, packet->falcon.rsn,
                  falcon::PacketType::kPullRequest)) {
            // No available resources for non-HoL network requests.
            return absl::ResourceExhaustedError(
                "Not enough FALCON credits for non-HoL network requests.");
          }
          request_credits = ComputeIncomingPullRequestResources(
              packet->falcon.payload_length);
          break;
      }
      break;
    case falcon::PacketType::kPullData:
      switch (direction) {
        case PacketDirection::kOutgoing:
          request_credits = ComputeOutgoingPullDataResources(
              packet->rdma.data_length, packet->metadata.sgl_length);
          break;
        case PacketDirection::kIncoming:
          if (config->resource_reservation_mode() ==
              FalconConfig::FIRST_PHASE_RESERVATION) {
            // Given that resources for this transaction are proactively
            // reserved, we don't need to reserve credits now.
            ASSIGN_OR_RETURN(
                TransactionMetadata* const transaction,
                connection_state->GetTransaction(
                    {packet->rdma.rsn, TransactionLocation::kInitiator}));
            return transaction->resources_reserved;
          }
          break;
      }
      break;
    case falcon::PacketType::kPushRequest:
      switch (direction) {
        case PacketDirection::kOutgoing:
          if (config->resource_reservation_mode() ==
              FalconConfig::FIRST_PHASE_RESERVATION) {
            // Proactively reserve resources for future incoming push grant and
            // outgoing push solicited data transactions along with the outgoing
            // push request.
            request_credits = ComputeOutgoingPushRequestResources(
                packet->rdma.data_length, packet->metadata.sgl_length);
            request_credits += ComputeIncomingPushGrantResources();
            request_credits += ComputeOutgoingPushSolicitedDataResources(
                packet->rdma.data_length, packet->metadata.sgl_length);
          }
          break;
        case PacketDirection::kIncoming:
          // Incoming requests should be subject to green-yellow-red checking
          // regardless of reservation mode.
          if (!MeetsNetworkRequestAdmissionCriteria(
                  packet->falcon.dest_cid, packet->falcon.rsn,
                  falcon::PacketType::kPushRequest)) {
            // No available resources for non-HoL network requests.
            return absl::ResourceExhaustedError(
                "Not enough FALCON credits for non-HoL network requests.");
          }

          if (config->resource_reservation_mode() ==
              FalconConfig::FIRST_PHASE_RESERVATION) {
            // Proactively reserve resources for future outgoing push grant and
            // incoming push solicited data transactions along with the incoming
            // push request.
            request_credits = ComputeIncomingPushRequestResources();
            request_credits += ComputeOutgoingPushGrantResources();
            request_credits += ComputeIncomingPushSolicitedDataResources(
                packet->falcon.request_length);
          }
          break;
      }
      break;
    case falcon::PacketType::kPushGrant:
      switch (direction) {
        case PacketDirection::kOutgoing:
          if (config->resource_reservation_mode() ==
              FalconConfig::FIRST_PHASE_RESERVATION) {
            // Given that resources for this transaction are proactively
            // reserved, we don't need to reserve credits now.
            ASSIGN_OR_RETURN(
                TransactionMetadata* const transaction,
                connection_state->GetTransaction(
                    {packet->falcon.rsn, TransactionLocation::kTarget}));
            return transaction->resources_reserved;
          }
          break;
        case PacketDirection::kIncoming:
          if (config->resource_reservation_mode() ==
              FalconConfig::FIRST_PHASE_RESERVATION) {
            // Given that resources for this transaction are proactively
            // reserved, we don't need to reserve credits now.
            ASSIGN_OR_RETURN(
                TransactionMetadata* const transaction,
                connection_state->GetTransaction(
                    {packet->falcon.rsn, TransactionLocation::kInitiator}));
            return transaction->resources_reserved;
          }
          break;
      }
      break;
    case falcon::PacketType::kPushSolicitedData:
      switch (direction) {
        case PacketDirection::kOutgoing:
          if (config->resource_reservation_mode() ==
              FalconConfig::FIRST_PHASE_RESERVATION) {
            // Given that resources for this transaction are proactively
            // reserved, we don't need to reserve credits now.
            ASSIGN_OR_RETURN(
                TransactionMetadata* const transaction,
                connection_state->GetTransaction(
                    {packet->falcon.rsn, TransactionLocation::kInitiator}));
            return transaction->resources_reserved;
          }
          break;
        case PacketDirection::kIncoming:
          ASSIGN_OR_RETURN(
              TransactionMetadata* const transaction,
              connection_state->GetTransaction(
                  {packet->falcon.rsn, TransactionLocation::kTarget}));
          if (config->resource_reservation_mode() ==
              FalconConfig::FIRST_PHASE_RESERVATION) {
            // Given that resources for this transaction are proactively
            // reserved, we don't need to reserve credits now.
            return transaction->resources_reserved;
          }
          break;
      }
      break;
    case falcon::PacketType::kPushUnsolicitedData:
      switch (direction) {
        case PacketDirection::kOutgoing:
          request_credits = ComputeOutgoingPushUnsolicitedDataResources(
              packet->rdma.data_length, packet->metadata.sgl_length);
          request_credits +=
              ComputeIncomingPushUnsolicitedCompletionResources();
          break;
        case PacketDirection::kIncoming:
          if (!MeetsNetworkRequestAdmissionCriteria(
                  packet->falcon.dest_cid, packet->falcon.rsn,
                  falcon::PacketType::kPushUnsolicitedData)) {
            // No available resources for non-HoL network requests.
            return absl::ResourceExhaustedError(
                "Not enough FALCON credits for non-HoL network requestssss.");
          }
          request_credits = ComputeIncomingPushUnsolicitedDataResources(
              packet->falcon.payload_length);
          break;
      }
      break;
    case falcon::PacketType::kAck:
      LOG(FATAL) << "No resources to be reserved for ACKs";
      break;
    case falcon::PacketType::kNack:
      LOG(FATAL) << "No resources to be reserved for NACKs";
      break;
    case falcon::PacketType::kBack:
      LOG(FATAL) << "No resources to be reserved for BACKs";
      break;
    case falcon::PacketType::kEack:
      LOG(FATAL) << "No resources to be reserved for EACKs";
      break;
    case falcon::PacketType::kResync:
      LOG(FATAL) << "No resources to be reserved for Resyncs.";
      break;
    case falcon::PacketType::kInvalid:
      LOG(FATAL) << "No resources to be reserved for invalid packets.";
      break;
  }

  if (request_credits <= global_credits_) {
    if (reserve_resources) {
      global_credits_ -= request_credits;
      if (request_credits.IsInitialized() &&
          falcon_->get_config()->enable_rx_buffer_occupancy_reflection()) {
        UpdateNetworkRegionEmaOccupancy();
      }
      falcon_->get_stats_manager()->UpdateResourceCounters(
          scid, request_credits, false);
      UpdateResourceAvailabilityCounters();
      CheckUlpRequestXoff();
    }
    return absl::OkStatus();
  }

  // Returns resource exhausted error in case no more credits are available.
  return absl::ResourceExhaustedError("Not enough FALCON credits.");
}

absl::Status ProtocolResourceManager::ReleaseResources(
    uint32_t scid, const TransactionKey& transaction_key,
    falcon::PacketType type) {
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  const FalconConfig* const config = falcon_->get_config();
  // Return without doing credit release in this mode.
  if (config->resource_reservation_mode() == FalconConfig::BYPASS_RESERVATION) {
    return absl::OkStatus();
  }

  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(scid));
  ASSIGN_OR_RETURN(TransactionMetadata* const transaction,
                   connection_state->GetTransaction(transaction_key));
  ASSIGN_OR_RETURN(PacketMetadata* const packet_metadata,
                   transaction->GetPacketMetadata(type));

  // Return immediately in case resources corresponding to this transaction have
  // already been released. This happens when a transaction times out and its
  // resources are taken back.
  if (!packet_metadata->resources_reserved.ok()) {
    return packet_metadata->resources_reserved;
  }

  VLOG(2) << "[" << falcon_->get_host_id() << ": "
          << falcon_->get_environment()->ElapsedTime() << "][" << scid << ", "
          << transaction_key.rsn << ", " << static_cast<int>(type) << "] "
          << "Releasing resources for the FALCON transaction.";

  FalconResourceCredits release_credits;
  FalconCredit rdma_managed_resource_credits;
  switch (type) {
    case falcon::PacketType::kPullRequest:
      switch (packet_metadata->direction) {
        case PacketDirection::kOutgoing:
          // TX resources corresponding to a Pull transaction are released when
          // we receive an ACK from the network corresponding to the Pull
          // Request transaction. RX resources are released separately.
          release_credits = ComputeOutgoingPullRequestResources(
              packet_metadata->data_length, packet_metadata->sgl_length);
          break;
        case PacketDirection::kIncoming:
          // RX resources corresponding to a Pull transaction are released when
          // FALCON receives an ACK from ULP corresponding to a Pull Request.
          release_credits = ComputeIncomingPullRequestResources(
              packet_metadata->payload_length);
          break;
      }
      break;
    case falcon::PacketType::kPullData:
      switch (packet_metadata->direction) {
        case PacketDirection::kOutgoing:
          // TX resources corresponding to a Pull transaction are released when
          // FALCON receives an ACK from the network corresponding to the Pull
          // Data transaction.
          release_credits = ComputeOutgoingPullDataResources(
              packet_metadata->data_length, packet_metadata->sgl_length);
          // Also, the credits corresponding to RDMA managed TX resources need
          // to given back to RDMA.
          rdma_managed_resource_credits = release_credits;
          break;
        case PacketDirection::kIncoming:
          // RX resources corresponding to a Pull transaction are released when
          // FALCON delivers the Pull response to the ULP.
          release_credits =
              ComputeIncomingPullDataResources(packet_metadata->payload_length);
          // RDMA managed resource credits corresponding to both RX and TX of a
          // Pull transaction need to be returned to RDMA.
          ASSIGN_OR_RETURN(
              PacketMetadata* const pull_request_transaction,
              transaction->GetPacketMetadata(falcon::PacketType::kPullRequest));
          auto transaction_resources_credits =
              ComputeOutgoingPullRequestResources(
                  pull_request_transaction->data_length,
                  pull_request_transaction->sgl_length);
          transaction_resources_credits += release_credits;
          rdma_managed_resource_credits = transaction_resources_credits;
      }
      break;
    // In case of a push request, in the kFirstPhaseReservation resource
    // reservation mode, we don't release any resources as its resources are
    // reused by the future push data transactions.
    case falcon::PacketType::kPushRequest:
      switch (packet_metadata->direction) {
        case PacketDirection::kOutgoing:
          break;
        case PacketDirection::kIncoming:
          break;
      }
      break;

    case falcon::PacketType::kPushGrant:
      switch (packet_metadata->direction) {
        case PacketDirection::kOutgoing:
          // TX resources of corresponding to a Push Grant are released when
          // FALCON receive an ACK corresponding to it from the network.
          release_credits = ComputeOutgoingPushGrantResources();
          break;
        case PacketDirection::kIncoming:
          // We do not release RX resources corresponding to the Push Grant as
          // it is reused later to hold the completion.
          break;
      }
      break;
    case falcon::PacketType::kPushSolicitedData:
      switch (packet_metadata->direction) {
        case PacketDirection::kOutgoing:
          release_credits = ComputeOutgoingPushSolicitedDataResources(
              packet_metadata->data_length, packet_metadata->sgl_length);
          // Release resources corresponding to the completion as well as TX
          // resources corresponding to the push data together to minimize QP
          // context cache bandwidth.
          release_credits += ComputeIncomingPushGrantResources();
          // While releasing resources for a push solicited data transaction, we
          // should also release resources corresponding to the push request as
          // its resources were not released earlier due to them being reused by
          // this transaction.
          if (config->resource_reservation_mode() ==
              FalconConfig::FIRST_PHASE_RESERVATION) {
            release_credits += ComputeOutgoingPushRequestResources(
                packet_metadata->data_length, packet_metadata->sgl_length);
          }
          // Also, need to return credits corresponding to RDMA managed
          // resources.
          rdma_managed_resource_credits = release_credits;
          break;
        case PacketDirection::kIncoming:
          release_credits = ComputeIncomingPushSolicitedDataResources(
              packet_metadata->payload_length);
          // While releasing resources for a push solicited data transaction, we
          // should also release resources corresponding to the push request as
          // its resources were not released earlier due to them being reused by
          // this transaction.
          if (config->resource_reservation_mode() ==
              FalconConfig::FIRST_PHASE_RESERVATION) {
            release_credits += ComputeIncomingPushRequestResources();
          }
          break;
      }
      break;
    case falcon::PacketType::kPushUnsolicitedData:
      switch (packet_metadata->direction) {
        case PacketDirection::kOutgoing:
          // TX resources corresponding to push unsolicited data as well as the
          // RX resources corresponding to the completion are both released
          // together, when FALCON delivers the completion to ULP. They are
          // released both at the same time to minimize QP context cache
          // bandwidth.
          release_credits = ComputeOutgoingPushUnsolicitedDataResources(
              packet_metadata->data_length, packet_metadata->sgl_length);
          release_credits +=
              ComputeIncomingPushUnsolicitedCompletionResources();
          // Also, need to return credits corresponding to RDMA managed
          // resources.
          rdma_managed_resource_credits = release_credits;
          break;
        case PacketDirection::kIncoming:
          // RX resources corresponding to a Push Unsolicited transaction are
          // released when we get an ACK from ULP corresponding to the
          // unsolicited transaction.
          release_credits = ComputeIncomingPushUnsolicitedDataResources(
              packet_metadata->payload_length);
          break;
      }
      break;
    case falcon::PacketType::kAck:
      LOG(FATAL) << "No resources to be released for ACKs";
      break;
    case falcon::PacketType::kNack:
      LOG(FATAL) << "No resources to be released for NACKs";
      break;
    case falcon::PacketType::kBack:
      LOG(FATAL) << "No resources to be released for NACKs";
      break;
    case falcon::PacketType::kEack:
      LOG(FATAL) << "No resources to be released for NACKs";
      break;
    case falcon::PacketType::kResync:
      LOG(FATAL) << "No resources to be released for Resyncs.";
      break;
    case falcon::PacketType::kInvalid:
      LOG(FATAL) << "No resources to be released for invalid packets.";
      break;
  }
  global_credits_ += release_credits;
  falcon_->get_stats_manager()->UpdateResourceCounters(scid, release_credits,
                                                       true);
  UpdateResourceAvailabilityCounters();

  if (release_credits.IsInitialized() &&
      falcon_->get_config()->enable_rx_buffer_occupancy_reflection()) {
    UpdateNetworkRegionEmaOccupancy();
  }

  ReturnRdmaManagedFalconResourceCredits(connection_state, transaction_key,
                                         rdma_managed_resource_credits);

  CheckUlpRequestXoff();
  packet_metadata->resources_reserved =
      absl::ResourceExhaustedError("Transaction does not hold FALCON credits");
  return absl::OkStatus();
}

// Returns credits corresponding to RDMA managed resources to RDMA, if required.
// Credits are returned after sending Pull Data and completions at the
// initiator, and when we receive ACK corresponding to Pull Data at the target.
void ProtocolResourceManager::ReturnRdmaManagedFalconResourceCredits(
    ConnectionState* const connection_state,
    const TransactionKey& transaction_key,
    FalconCredit rdma_managed_resource_credits) {
  if (rdma_managed_resource_credits.IsInitialized()) {
    // Get a handle on the received packet context corresponding to this
    // transaction (to access the QP ID).
    CHECK_OK_THEN_ASSIGN(
        auto pkt_ctx,
        connection_state->rx_reliability_metadata.GetReceivedPacketContext(
            transaction_key));
    // Return credits to RDMA.
    falcon_->get_rdma_model()->ReturnFalconCredit(
        pkt_ctx->qp_id, rdma_managed_resource_credits);
  }
}

// Returns the number of credits required for an outgoing pull request
// transaction. Buffer credit calculated based on data_length and sgl_length.
FalconResourceCredits
ProtocolResourceManager::ComputeOutgoingPullRequestResources(
    uint32_t data_length, uint32_t sgl_length) {
  FalconResourceCredits credits;
  credits.tx_packet_credits.ulp_requests = 1;
  credits.tx_buffer_credits.ulp_requests = CalculateFalconTxBufferCredits(
      data_length, sgl_length,
      falcon_->get_config()->tx_buffer_minimum_allocation_unit());
  return credits;
}

// Returns the number of credits required for an incoming pull request
// transaction. Buffer credit calculated based on payload_length of incoming
// transaction.
FalconResourceCredits
ProtocolResourceManager::ComputeIncomingPullRequestResources(
    uint32_t payload_length) {
  FalconResourceCredits credits;
  credits.rx_packet_credits.network_requests = 1;
  credits.rx_buffer_credits.network_requests = CalculateFalconRxBufferCredits(
      payload_length,
      falcon_->get_config()->rx_buffer_minimum_allocation_unit());
  return credits;
}

// Returns the number of credits required for an outgoing pull data transaction.
// Buffer credit calculated based on data_length and sgl_length.
FalconResourceCredits ProtocolResourceManager::ComputeOutgoingPullDataResources(
    uint32_t data_length, uint32_t sgl_length) {
  FalconResourceCredits credits;
  credits.tx_packet_credits.ulp_data = 1;
  credits.tx_buffer_credits.ulp_data = CalculateFalconTxBufferCredits(
      data_length, sgl_length,
      falcon_->get_config()->tx_buffer_minimum_allocation_unit());
  return credits;
}

// Returns the number of credits required for an incoming pull data transaction.
// Buffer credit calculated based on FALCON payload length of the incoming pull
// data. If the resources are proactive reserved (as is the case with FALCON),
// then the expected length is passed as request_length in ULP request.
// Otherwise, we use payload_length obtained from incoming packet.
FalconResourceCredits ProtocolResourceManager::ComputeIncomingPullDataResources(
    uint32_t request_length) {
  FalconResourceCredits credits;
  credits.rx_packet_credits.ulp_requests = 1;
  credits.rx_buffer_credits.ulp_requests = CalculateFalconRxBufferCredits(
      request_length,
      falcon_->get_config()->rx_buffer_minimum_allocation_unit());
  return credits;
}

// Returns the number of credits required for an outgoing push request
// transaction. Buffer credit calculated based on data_length and sgl_length.
FalconResourceCredits
ProtocolResourceManager::ComputeOutgoingPushRequestResources(
    uint32_t data_length, uint32_t sgl_length) {
  FalconResourceCredits credits;
  credits.tx_packet_credits.ulp_requests = 1;
  credits.tx_buffer_credits.ulp_requests = CalculateFalconTxBufferCredits(
      data_length, sgl_length,
      falcon_->get_config()->tx_buffer_minimum_allocation_unit());
  return credits;
}

// Returns the number of credits required for an incoming push request
// transaction. Buffer credits are 0 as this a FALCON internal transaction.
FalconResourceCredits
ProtocolResourceManager::ComputeIncomingPushRequestResources() {
  FalconResourceCredits credits;
  credits.rx_packet_credits.network_requests = 1;
  credits.rx_buffer_credits.network_requests = 0;
  return credits;
}

// Returns the number of credits required for an outgoing push grant
// transaction. Buffer credits are 0 as this a FALCON internal transaction.
FalconResourceCredits
ProtocolResourceManager::ComputeOutgoingPushGrantResources() {
  FalconResourceCredits credits;
  credits.tx_packet_credits.network_requests = 1;
  credits.tx_buffer_credits.network_requests = 0;
  return credits;
}

// Returns the number of credits required for an incoming push grant
// transaction. Buffer credits are 0 as this a FALCON internal transaction.
FalconResourceCredits
ProtocolResourceManager::ComputeIncomingPushGrantResources() {
  FalconResourceCredits credits;
  credits.rx_packet_credits.ulp_requests = 1;
  credits.rx_buffer_credits.ulp_requests = 0;
  return credits;
}

// Returns the number of credits required for an outgoing push solicited data
// transaction. In case the reservation mode is kFirstPhaseReservation
// (corresponds to FALCON behavior), then we reduce the required credits as the
// transaction can reuse resources from the earlier PushRequest transaction.
// Buffer credit calculated based on data_length and sgl_length.
FalconResourceCredits
ProtocolResourceManager::ComputeOutgoingPushSolicitedDataResources(
    uint32_t data_length, uint32_t sgl_length) {
  FalconResourceCredits credits;

  const FalconConfig* config = falcon_->get_config();
  credits.tx_packet_credits.ulp_requests = 1;
  credits.tx_buffer_credits.ulp_requests = CalculateFalconTxBufferCredits(
      data_length, sgl_length,
      falcon_->get_config()->tx_buffer_minimum_allocation_unit());
  if (config->resource_reservation_mode() ==
      FalconConfig::FIRST_PHASE_RESERVATION) {
    credits -= ComputeOutgoingPushRequestResources(data_length, sgl_length);
  }
  return credits;
}

// Returns the number of credits required for holding an incoming Push
// Unsolicited Data Completion. Buffer credits are 0 as this a FALCON internal
// transaction.
FalconResourceCredits
ProtocolResourceManager::ComputeIncomingPushUnsolicitedCompletionResources() {
  FalconResourceCredits credits;
  credits.rx_packet_credits.ulp_requests = 1;
  credits.rx_buffer_credits.ulp_requests = 0;
  return credits;
}

// Returns the number of credits required for an incoming push solicited data
// transaction.
// Buffer credit calculated based on FALCON payload length of the incoming push
// data. If the resources are proactive reserved (as is the case with FALCON),
// then the expected length is passed as request_length obtained from Push
// request. Otherwise, we use payload_length obtained from incoming packet.
FalconResourceCredits
ProtocolResourceManager::ComputeIncomingPushSolicitedDataResources(
    uint32_t request_length) {
  FalconResourceCredits credits;
  const FalconConfig* config = falcon_->get_config();
  credits.rx_packet_credits.network_requests = 1;
  credits.rx_buffer_credits.network_requests = CalculateFalconRxBufferCredits(
      request_length,
      falcon_->get_config()->rx_buffer_minimum_allocation_unit());
  if (config->resource_reservation_mode() ==
      FalconConfig::FIRST_PHASE_RESERVATION) {
    credits -= ComputeIncomingPushRequestResources();
  }
  return credits;
}

// Returns the number of credits required for an outgoing push unsolicited data
// transaction. Buffer credit calculated based on data_length and sgl_length.
FalconResourceCredits
ProtocolResourceManager::ComputeOutgoingPushUnsolicitedDataResources(
    uint32_t data_length, uint32_t sgl_length) {
  FalconResourceCredits credits;
  credits.tx_packet_credits.ulp_requests = 1;
  credits.tx_buffer_credits.ulp_requests = CalculateFalconTxBufferCredits(
      data_length, sgl_length,
      falcon_->get_config()->tx_buffer_minimum_allocation_unit());
  return credits;
}

// Returns the number of credits required for an incoming push unsolicited data
// transaction. Buffer credit calculated based on request_length (which is also
// equal to payload_length in case of unsolicited writes).
FalconResourceCredits
ProtocolResourceManager::ComputeIncomingPushUnsolicitedDataResources(
    uint32_t request_length) {
  FalconResourceCredits credits;
  credits.rx_packet_credits.network_requests = 1;
  credits.rx_buffer_credits.network_requests = CalculateFalconRxBufferCredits(
      request_length,
      falcon_->get_config()->rx_buffer_minimum_allocation_unit());
  return credits;
}

void ProtocolResourceManager::CheckUlpRequestXoff() {
  bool request_xoff = !(request_xoff_limit_ <= global_credits_);
  bool global_xoff = !(global_xoff_limit_ <= global_credits_);

  // If the xoff status has changed since last time, send it to RDMA.
  if (request_xoff != request_xoff_ || global_xoff != global_xoff_) {
    request_xoff_ = request_xoff;
    global_xoff_ = global_xoff;
    falcon_->get_rdma_model()->SetXoff(request_xoff_, global_xoff_);
  }
}

// Checks whether the incoming request meets the admission criteria or not. This
// is required to prevent deadlocks. Specifically, the RX network requests
// region (buffer and packet) is divided into 3 zones: green, yellow and red -
// 1. Green Zone: Allow any incoming network request
// 2. Yellow Zone: Allow any incoming HoL network request
// 3. Red Zone: Allow only HoL pull request or push unsolicited data (i.e. do
// not allow HoL push requests).
// Note: Though push solicited data packets consume resources from the RX
// network request buffer space, we do not need to check whether an incoming
// push solicited data packet meets this admission criteria as resources
// corresponding to it were reserved when we received the corresponding push
// request before.
bool ProtocolResourceManager::MeetsNetworkRequestAdmissionCriteria(
    uint32_t cid, uint32_t rsn, falcon::PacketType type) {
  // Infer the occupancy zone of the network request region - it corresponds to
  // the higher occupancy zone among the packet and buffer resources.
  RxNetworkRequestZone rx_network_request_occupancy_zone;
  rx_network_request_occupancy_zone =
      std::max({GetNetworkRequestRxBufferPoolOccupancyZone(),
                GetNetworkRequestRxPacketPoolOccupancyZone(),
                GetNetworkRequestTxPacketPoolOccupancyZone()});

  switch (rx_network_request_occupancy_zone) {
    case RxNetworkRequestZone::kGreen:
      // Allow any incoming network request.
      return true;
    case RxNetworkRequestZone::kYellow:
      // Allow any incoming HoL network request.
      return falcon_->get_buffer_reorder_engine()->IsHeadOfLineNetworkRequest(
          cid, rsn);
    case RxNetworkRequestZone::kRed:
      // Allow only HoL pull request or push unsolicited data.
      if (type == falcon::PacketType::kPullRequest ||
          type == falcon::PacketType::kPushUnsolicitedData) {
        return falcon_->get_buffer_reorder_engine()->IsHeadOfLineNetworkRequest(
            cid, rsn);
      } else {
        return false;
      }
  }
}

// Returns the occupancy zone of the network requests RX buffer pool.
RxNetworkRequestZone
ProtocolResourceManager::GetNetworkRequestRxBufferPoolOccupancyZone() {
  // Get a handle on the FALCON configuration.
  const FalconConfig* const config = falcon_->get_config();
  // Calculate the current occupancy of the network requests pool.
  auto rx_network_requests_buffer_occupancy =
      config->resource_credits().rx_buffer_credits().network_requests() -
      global_credits_.rx_buffer_credits.network_requests;

  if (rx_network_requests_buffer_occupancy <
      config->falcon_network_requests_rx_buffer_pool_thresholds()
          .green_zone_end()) {
    return RxNetworkRequestZone::kGreen;
  } else if (rx_network_requests_buffer_occupancy <
             config->falcon_network_requests_rx_buffer_pool_thresholds()
                 .yellow_zone_end()) {
    return RxNetworkRequestZone::kYellow;
  } else {
    return RxNetworkRequestZone::kRed;
  }
}

// Returns the occupancy zone of the network requests RX packet pool.
RxNetworkRequestZone
ProtocolResourceManager::GetNetworkRequestRxPacketPoolOccupancyZone() {
  // Get a handle on the FALCON configuration.
  const FalconConfig* const config = falcon_->get_config();
  // Calculate the current occupancy of the network requests pool.
  auto rx_network_requests_packet_pool_occupancy =
      config->resource_credits().rx_packet_credits().network_requests() -
      global_credits_.rx_packet_credits.network_requests;

  if (rx_network_requests_packet_pool_occupancy <
      config->falcon_network_requests_rx_packet_pool_thresholds()
          .green_zone_end()) {
    return RxNetworkRequestZone::kGreen;
  } else if (rx_network_requests_packet_pool_occupancy <
             config->falcon_network_requests_rx_packet_pool_thresholds()
                 .yellow_zone_end()) {
    return RxNetworkRequestZone::kYellow;
  } else {
    return RxNetworkRequestZone::kRed;
  }
}

// Returns the occupancy zone of the network requests TX packet pool.
RxNetworkRequestZone
ProtocolResourceManager::GetNetworkRequestTxPacketPoolOccupancyZone() {
  // Get a handle on the FALCON configuration.
  const FalconConfig* const config = falcon_->get_config();
  // Calculate the current occupancy of the network requests pool.
  auto tx_network_requests_packet_pool_occupancy =
      config->resource_credits().tx_packet_credits().network_requests() -
      global_credits_.tx_packet_credits.network_requests;

  if (tx_network_requests_packet_pool_occupancy <
      config->falcon_network_requests_tx_packet_pool_thresholds()
          .green_zone_end()) {
    return RxNetworkRequestZone::kGreen;
  } else if (tx_network_requests_packet_pool_occupancy <
             config->falcon_network_requests_tx_packet_pool_thresholds()
                 .yellow_zone_end()) {
    return RxNetworkRequestZone::kYellow;
  } else {
    return RxNetworkRequestZone::kRed;
  }
}

// Updates the EMA values corresponding to the network region resources whenever
// resources are reserved or released. Specifically, the EMA values are updated
// using the previous EMA value, EMA coefficient and current occupancy.
void ProtocolResourceManager::UpdateNetworkRegionEmaOccupancy() {
  // Get a handle on the FALCON configuration.
  const FalconConfig* const config = falcon_->get_config();

  // Get a handle on current used up network region resources relevant to target
  // buffer occupancy (rx packet pool, rx buffer pool, tx packet pool).
  std::array<int32_t, 3> current_occupancies{
      config->resource_credits().rx_buffer_credits().network_requests() -
          global_credits_.rx_buffer_credits.network_requests,
      config->resource_credits().rx_packet_credits().network_requests() -
          global_credits_.rx_packet_credits.network_requests,
      config->resource_credits().tx_packet_credits().network_requests() -
          global_credits_.tx_packet_credits.network_requests,
  };

  // Get a handle on the corresponding EMA values of the resources
  std::array<uint32_t*, 3> ema_occupancies{
      &network_region_ema_.rx_buffer_pool_occupancy_ema,
      &network_region_ema_.rx_packet_pool_occupancy_ema,
      &network_region_ema_.tx_packet_pool_occupancy_ema,
  };

  // Get a handle on the corresponding EMA coefficients of the resources
  std::array<uint32_t, 3> ema_coefficients{
      config->ema_coefficients().rx_buffer(),
      config->ema_coefficients().rx_context(),
      config->ema_coefficients().tx_context(),
  };

  // Iterate over the resources and update their EMA values.
  for (int i = 0; i < 3; i++) {
    auto& current_occupancy = current_occupancies[i];
    auto* ema_occupancy = ema_occupancies[i];
    auto ema_coefficient =
        std::pow(2, -1 * static_cast<double>(ema_coefficients[i]));
    if (*ema_occupancy == 0) {
      *ema_occupancy = falcon_rue::UintToFixed<uint32_t, uint32_t>(
          current_occupancy, kEmaOccupancyFractionalBits);
    } else {
      double float_ema_occupancy = falcon_rue::FixedToFloat<uint32_t, double>(
          *ema_occupancy, kEmaOccupancyFractionalBits);
      float_ema_occupancy = ema_coefficient * current_occupancy +
                            (1 - ema_coefficient) * float_ema_occupancy;
      // Record EMA resource occupancy values in stats collection framework.
      StatisticCollectionInterface* stats_collector =
          falcon_->get_stats_collector();
      if (collect_ema_occupancy_ && stats_collector) {
        CHECK_OK(stats_collector->UpdateStatistic(
            std::string(kStatVectorResourceManagerEma[i]), float_ema_occupancy,
            StatisticsCollectionConfig::TIME_SERIES_STAT));
      }
      *ema_occupancy = falcon_rue::FloatToFixed<double, uint32_t>(
          float_ema_occupancy, kEmaOccupancyFractionalBits);
    }
  }

  // Update the quantized value of the network region occupancy.
  UpdateQuantizedNetworkRegionOccupancy();
}

// Updates the qantized network region occupancy to the max(rx_pkt, rx_buf,
// tx_pkt).
void ProtocolResourceManager::UpdateQuantizedNetworkRegionOccupancy() {
  // Get a handle on the FALCON configuration.
  const FalconConfig* const config = falcon_->get_config();

  // Get a handle on the corresponding EMA values of the resources
  std::array<uint32_t*, 3> ema_occupancies{
      &network_region_ema_.rx_buffer_pool_occupancy_ema,
      &network_region_ema_.rx_packet_pool_occupancy_ema,
      &network_region_ema_.tx_packet_pool_occupancy_ema,
  };

  // Get a handle on the quantization tables of the resources
  std::array<const FalconConfig::TargetBufferOccupancyQuantizationTables::
                 QuantizationTable*,
             3>
      quantization_tables{&config->quantization_tables().rx_buffer(),
                          &config->quantization_tables().rx_context(),
                          &config->quantization_tables().tx_context()};

  // Holds the occupancy level of the resource that has the maximum quantized
  // value.
  uint16_t max_quantized_value = 0;

  // Iterate over the resources and find the max quantized value among the three
  // resources.
  for (int i = 0; i < 3; i++) {
    auto* quantization_table = quantization_tables[i];
    // Get the integer portion of the EMA occupancy represent in fxied-point
    // representation.
    uint16_t integer_ema_occupancy =
        falcon_rue::FixedToUint<uint32_t, uint16_t>(
            *ema_occupancies[i], kEmaOccupancyFractionalBits);

    // Find the 5-bit quantized value of the 16-bit EMA occupancy.
    uint32_t quantized_value;
    if (integer_ema_occupancy <
        quantization_table->quantization_level_0_threshold()) {
      quantized_value = 0;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_1_threshold()) {
      quantized_value = 1;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_2_threshold()) {
      quantized_value = 2;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_3_threshold()) {
      quantized_value = 3;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_4_threshold()) {
      quantized_value = 4;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_5_threshold()) {
      quantized_value = 5;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_6_threshold()) {
      quantized_value = 6;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_7_threshold()) {
      quantized_value = 7;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_8_threshold()) {
      quantized_value = 8;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_9_threshold()) {
      quantized_value = 9;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_10_threshold()) {
      quantized_value = 10;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_11_threshold()) {
      quantized_value = 11;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_12_threshold()) {
      quantized_value = 12;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_13_threshold()) {
      quantized_value = 13;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_14_threshold()) {
      quantized_value = 14;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_15_threshold()) {
      quantized_value = 15;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_16_threshold()) {
      quantized_value = 16;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_17_threshold()) {
      quantized_value = 17;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_18_threshold()) {
      quantized_value = 18;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_19_threshold()) {
      quantized_value = 19;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_20_threshold()) {
      quantized_value = 20;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_21_threshold()) {
      quantized_value = 21;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_22_threshold()) {
      quantized_value = 22;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_23_threshold()) {
      quantized_value = 23;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_24_threshold()) {
      quantized_value = 24;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_25_threshold()) {
      quantized_value = 25;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_26_threshold()) {
      quantized_value = 26;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_27_threshold()) {
      quantized_value = 27;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_28_threshold()) {
      quantized_value = 28;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_29_threshold()) {
      quantized_value = 29;
    } else if (integer_ema_occupancy <
               quantization_table->quantization_level_30_threshold()) {
      quantized_value = 30;
    } else {
      quantized_value = 31;
    }

    // Update the maximum quantized value among three resources, if applicable.
    if (quantized_value > max_quantized_value) {
      max_quantized_value = quantized_value;
    }
  }
  network_region_occupancy_ = max_quantized_value;
  // Record quantized network region occupancy in stats collection framework.
  StatisticCollectionInterface* stats_collector =
      falcon_->get_stats_collector();
  if (collect_ema_occupancy_ && stats_collector) {
    CHECK_OK(stats_collector->UpdateStatistic(
        std::string(kStatVectorResourceManagerNetworkRegionQuantizedOccupancy),
        network_region_occupancy_,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
}

// Testing function that updates EMA values of network region occupancies and
// returns them.
NetworkRegionEmaOccupancy
ProtocolResourceManager::UpdateNetworkRegionEmaOccupancyForTesting() {
  UpdateNetworkRegionEmaOccupancy();
  return network_region_ema_;
}

// Updates the resources availability to the stats collection framework.
void ProtocolResourceManager::UpdateResourceAvailabilityCounters() {
  StatisticCollectionInterface* stats_collector =
      falcon_->get_stats_collector();
  if (collect_resource_credit_timeline_ && stats_collector) {
    // Records TX Packet Credits available.
    CHECK_OK(stats_collector->UpdateStatistic(
        std::string(kStatsTxPktCreditsUlpReqs),
        global_credits_.tx_packet_credits.ulp_requests,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
    CHECK_OK(stats_collector->UpdateStatistic(
        std::string(kStatsTxPktCreditsUlpData),
        global_credits_.tx_packet_credits.ulp_data,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
    CHECK_OK(stats_collector->UpdateStatistic(
        std::string(kStatsTxPktCreditsNtwkReqs),
        global_credits_.tx_packet_credits.network_requests,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
    // Records TX Buffer Credits available.
    CHECK_OK(stats_collector->UpdateStatistic(
        std::string(kStatsTxBufferCreditsUlpReqs),
        global_credits_.tx_buffer_credits.ulp_requests,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
    CHECK_OK(stats_collector->UpdateStatistic(
        std::string(kStatsTxBufferCreditsUlpData),
        global_credits_.tx_buffer_credits.ulp_data,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
    CHECK_OK(stats_collector->UpdateStatistic(
        std::string(kStatsTxBufferCreditsNtwkReqs),
        global_credits_.tx_buffer_credits.network_requests,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
    // Records RX Packet Credits available.
    CHECK_OK(stats_collector->UpdateStatistic(
        std::string(kStatsRxPktCreditsUlpReqs),
        global_credits_.rx_packet_credits.ulp_requests,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
    CHECK_OK(stats_collector->UpdateStatistic(
        std::string(kStatsRxPktCreditsNtwkReqs),
        global_credits_.rx_packet_credits.network_requests,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
    // Records RX Buffer Credits available.
    CHECK_OK(stats_collector->UpdateStatistic(
        std::string(kStatsRxBufferCreditsUlpReqs),
        global_credits_.rx_buffer_credits.ulp_requests,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
    CHECK_OK(stats_collector->UpdateStatistic(
        std::string(kStatsRxBufferCreditsNtwkReqs),
        global_credits_.rx_buffer_credits.network_requests,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
}

}  // namespace isekai
