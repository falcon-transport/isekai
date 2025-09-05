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

#include "isekai/host/falcon/gen1/admission_control_manager.h"

#include <cstdint>
#include <memory>
#include <type_traits>

#include "absl/status/status.h"
#include "absl/strings/substitute.h"
#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_histograms.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"

namespace isekai {
namespace {
// String constants for recording solicitation window occupancy time series.
// Flag: enable_per_connection_solicitation_counters
static constexpr std::string_view kStatVectorSolicitationWindowOccupancyBytes =
    "falcon.solicitation_window_occupied_bytes";
}  // namespace

ProtocolAdmissionControlManager::ProtocolAdmissionControlManager(
    FalconModelInterface* falcon)
    : falcon_(falcon) {
  switch (falcon_->get_config()->admission_control_policy()) {
    case FalconConfig::RX_WINDOW_BASED:
      InitRxWindowAdmissionControl();
      break;
    case FalconConfig::RX_RATE_BASED:
      InitRxRateLimiter();
      break;
    case FalconConfig::TX_RATE_BASED:
      InitTxRateLimiter();
      break;
    case FalconConfig::RX_TX_RATE_BASED:
      InitRxRateLimiter();
      InitTxRateLimiter();
      break;
    case FalconConfig::RX_TX_RATE_RX_WINDOW_BASED:
      InitRxRateLimiter();
      InitTxRateLimiter();
      InitRxWindowAdmissionControl();
      break;
  }
  collect_solicitation_stats_ = falcon_->get_stats_manager()
                                    ->GetStatsConfig()
                                    .enable_solicitation_counters();
}

// Initializes the RX Window Admission Control.
void ProtocolAdmissionControlManager::InitRxWindowAdmissionControl() {
  window_admission_control_state_ =
      std::make_unique<WindowBasedAdmissionControlState>(
          falcon_->get_config()->admission_window_bytes());
}

// Initializes the RX Rate Limiter Metadata.
void ProtocolAdmissionControlManager::InitRxRateLimiter() {
  // Calculate the bytes (tokens) per second.
  uint64_t tokens_per_sec = falcon_->get_config()
                                ->rx_rate_limiter_metadata()
                                .solicitation_rate_gbps() *
                            kGiga / 8;
  // Setup the token bucket RX rate limiter.
  rx_solicitation_rate_limiter_token_bucket_ = std::make_unique<TokenBucket>(
      tokens_per_sec,
      absl::Nanoseconds(falcon_->get_config()
                            ->rx_rate_limiter_metadata()
                            .refill_interval_ns()),
      falcon_->get_config()->rx_rate_limiter_metadata().burst_size_bytes());
}

// Initializes the TX Rate Limiter Metadata.
void ProtocolAdmissionControlManager::InitTxRateLimiter() {
  // Calculate the bytes (tokens) per second.
  uint64_t tokens_per_sec = falcon_->get_config()
                                ->tx_rate_limiter_metadata()
                                .solicitation_rate_gbps() *
                            kGiga / 8;
  // Setup the token bucket RX rate limiter.
  tx_solicitation_rate_limiter_token_bucket_ = std::make_unique<TokenBucket>(
      tokens_per_sec,
      absl::Nanoseconds(falcon_->get_config()
                            ->tx_rate_limiter_metadata()
                            .refill_interval_ns()),
      falcon_->get_config()->tx_rate_limiter_metadata().burst_size_bytes());
}

// Checks to see if the transaction in question meets the admission control
// criteria.
bool ProtocolAdmissionControlManager::MeetsAdmissionControlCriteria(
    uint32_t scid, uint32_t rsn, falcon::PacketType type) {
  const bool is_incoming_packet = false;
  // Get a handle on the outstanding transaction.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(ConnectionState* const connection_state,
                       state_manager->PerformDirectLookup(scid));
  CHECK_OK_THEN_ASSIGN(
      TransactionMetadata * transaction,
      connection_state->GetTransaction(TransactionKey(
          rsn, GetTransactionLocation(type, is_incoming_packet))));
  CHECK_GE(transaction->request_length, 0);

  return MeetsAdmissionControlCriteria(transaction->request_length, type,
                                       connection_state);
}

bool ProtocolAdmissionControlManager::MeetsAdmissionControlCriteria(
    uint64_t request_length, falcon::PacketType type,
    const ConnectionState* connection_state) {
  bool admission_status = true;
  // Do admission control based on the admission control policy.
  switch (falcon_->get_config()->admission_control_policy()) {
    case FalconConfig::RX_WINDOW_BASED:
      admission_status =
          MeetsRxWindowBasedAdmissionCriteria(request_length, type);
      break;
    case FalconConfig::RX_RATE_BASED:
      admission_status =
          MeetsRxRateBasedAdmissionCriteria(request_length, type);
      break;
    case FalconConfig::TX_RATE_BASED:
      admission_status =
          MeetsTxRateBasedAdmissionCriteria(request_length, type);
      break;
    case FalconConfig::RX_TX_RATE_BASED: {
      auto rx_rate_limiter_admission_status =
          MeetsRxRateBasedAdmissionCriteria(request_length, type);
      auto tx_rate_limiter_admission_status =
          MeetsTxRateBasedAdmissionCriteria(request_length, type);
      admission_status =
          tx_rate_limiter_admission_status && rx_rate_limiter_admission_status;
    } break;
    case FalconConfig::RX_TX_RATE_RX_WINDOW_BASED: {
      auto rx_rate_limiter_admission_status =
          MeetsRxRateBasedAdmissionCriteria(request_length, type);
      auto tx_rate_limiter_admission_status =
          MeetsTxRateBasedAdmissionCriteria(request_length, type);
      auto rx_window_admission_status =
          MeetsRxWindowBasedAdmissionCriteria(request_length, type);
      admission_status = tx_rate_limiter_admission_status &&
                         rx_rate_limiter_admission_status &&
                         rx_window_admission_status;
    } break;
  }

  return admission_status;
}

// Returns false if there is not enough capacity left in the admission window.
bool ProtocolAdmissionControlManager::MeetsRxWindowBasedAdmissionCriteria(
    uint64_t request_length, falcon::PacketType type) {
  if (type == falcon::PacketType::kPullRequest ||
      type == falcon::PacketType::kPushGrant) {
    if (window_admission_control_state_->admission_window_bytes -
            window_admission_control_state_->solicited_data_bytes >=
        request_length) {
      return true;
    }
    return false;
  }
  return true;
}

// Returns false if there is not enough capacity left in the RX token bucket.
bool ProtocolAdmissionControlManager::MeetsRxRateBasedAdmissionCriteria(
    uint64_t request_length, falcon::PacketType type) {
  if (type == falcon::PacketType::kPullRequest ||
      type == falcon::PacketType::kPushGrant) {
    return rx_solicitation_rate_limiter_token_bucket_->AreTokensAvailable(
        request_length, falcon_->get_environment()->ElapsedTime());
  }
  return true;
}

// Returns false if there is not enough capacity left in the TX token bucket.
bool ProtocolAdmissionControlManager::MeetsTxRateBasedAdmissionCriteria(
    uint64_t request_length, falcon::PacketType type) {
  if (type == falcon::PacketType::kPushRequest) {
    return tx_solicitation_rate_limiter_token_bucket_->AreTokensAvailable(
        request_length, falcon_->get_environment()->ElapsedTime());
  }
  return true;
}

// Reserves resources related to admission control. Reserved resources
// correspond to bytes in the solicitation window in case of window-based
// solicitation and tokens in the token bucket in case of the rate
// limiters.
absl::Status ProtocolAdmissionControlManager::ReserveAdmissionControlResource(
    uint32_t scid, uint32_t rsn, falcon::PacketType type) {
  // Get a handle on the outstanding transaction.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(scid));
  ASSIGN_OR_RETURN(TransactionMetadata * transaction,
                   connection_state->GetTransaction(
                       TransactionKey(rsn, GetTransactionLocation(
                                               /*type=*/type,
                                               /*incoming=*/false))));
  CHECK_GE(transaction->request_length, 0);

  // Do admission control based on the admission control policy.
  switch (falcon_->get_config()->admission_control_policy()) {
    case FalconConfig::RX_WINDOW_BASED:
      ReserveRxWindowBasedAdmissionResources(transaction->request_length, type,
                                             scid, transaction);
      break;
    case FalconConfig::RX_RATE_BASED:
      ReserveRxRateBasedAdmissionResources(transaction->request_length, type);
      break;
    case FalconConfig::TX_RATE_BASED:
      ReserveTxRateBasedAdmissionResources(transaction->request_length, type);
      break;
    case FalconConfig::RX_TX_RATE_BASED:
      ReserveRxRateBasedAdmissionResources(transaction->request_length, type);
      ReserveTxRateBasedAdmissionResources(transaction->request_length, type);
      break;
    case FalconConfig::RX_TX_RATE_RX_WINDOW_BASED:
      ReserveRxRateBasedAdmissionResources(transaction->request_length, type);
      ReserveTxRateBasedAdmissionResources(transaction->request_length, type);
      ReserveRxWindowBasedAdmissionResources(transaction->request_length, type,
                                             scid, transaction);
  }

  return absl::OkStatus();
}

void ProtocolAdmissionControlManager::ReserveRxWindowBasedAdmissionResources(
    uint64_t request_length, falcon::PacketType type, uint32_t scid,
    TransactionMetadata* transaction) {
  // Reserve capacity in the solicitation window only for PullRequest and
  // PushGrant packets.
  if (type == falcon::PacketType::kPullRequest ||
      type == falcon::PacketType::kPushGrant) {
    uint64_t reserve_capacity = request_length;
    window_admission_control_state_->solicited_data_bytes += reserve_capacity;

    // Record solicitatoin window occupancy in stats collection framework.
    StatisticCollectionInterface* stats_collector =
        falcon_->get_stats_collector();
    if (collect_solicitation_stats_ && stats_collector) {
      CHECK_OK(stats_collector->UpdateStatistic(
          kStatVectorSolicitationWindowOccupancyBytes,
          window_admission_control_state_->solicited_data_bytes,
          StatisticsCollectionConfig::TIME_SERIES_STAT));
    }
    falcon_->get_stats_manager()->UpdateSolicitationCounters(
        scid, reserve_capacity, false);
    transaction->solicitation_window_reservation_timestamp =
        falcon_->get_environment()->ElapsedTime();
  }
}

void ProtocolAdmissionControlManager::ReserveRxRateBasedAdmissionResources(
    uint64_t request_length, falcon::PacketType type) {
  if (type == falcon::PacketType::kPullRequest ||
      type == falcon::PacketType::kPushGrant) {
    rx_solicitation_rate_limiter_token_bucket_->RequestTokens(request_length);
  }
}

void ProtocolAdmissionControlManager::ReserveTxRateBasedAdmissionResources(
    uint64_t request_length, falcon::PacketType type) {
  if (type == falcon::PacketType::kPushRequest) {
    tx_solicitation_rate_limiter_token_bucket_->RequestTokens(request_length);
  }
}

// Refunds the reserved resources once the transaction is complete, if
// necessary. This method is meaningful only for window-based admission
// control as it is closed-loop while the rate based ones are open loop and
// hence do not need to refund.
absl::Status ProtocolAdmissionControlManager::RefundAdmissionControlResource(
    uint32_t scid, const TransactionKey& transaction_key) {
  // Refund resources if window-based solicitation is enabled.
  auto admission_control_policy =
      falcon_->get_config()->admission_control_policy();
  if (admission_control_policy == FalconConfig::RX_WINDOW_BASED ||
      admission_control_policy == FalconConfig::RX_TX_RATE_RX_WINDOW_BASED) {
    ConnectionStateManager* const state_manager = falcon_->get_state_manager();
    ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                     state_manager->PerformDirectLookup(scid));
    ASSIGN_OR_RETURN(TransactionMetadata * transaction,
                     connection_state->GetTransaction(transaction_key));
    CHECK_GE(transaction->request_length, 0);

    uint64_t refund_capacity = transaction->request_length;
    window_admission_control_state_->solicited_data_bytes -= refund_capacity;

    // Record solicitation window counters in stats collection framework.
    StatisticCollectionInterface* stats_collector =
        falcon_->get_stats_collector();
    if (collect_solicitation_stats_ && stats_collector) {
      CHECK_OK(stats_collector->UpdateStatistic(
          kStatVectorSolicitationWindowOccupancyBytes,
          window_admission_control_state_->solicited_data_bytes,
          StatisticsCollectionConfig::TIME_SERIES_STAT));
    }
    falcon_->get_stats_manager()->UpdateSolicitationCounters(
        scid, refund_capacity, true);
    absl::Duration now = falcon_->get_environment()->ElapsedTime();
    double latency = absl::ToDoubleNanoseconds(
        now - transaction->solicitation_window_reservation_timestamp);
    switch (transaction->type) {
      case TransactionType::kPull:
        falcon_->get_histogram_collector()->Add(
            PullLatencyTypes::kTxNetworkRequestToTxUlpData, latency);
        break;
      case TransactionType::kPushSolicited:
        falcon_->get_histogram_collector()->Add(
            PushSolicitedLatencyTypes::kTxNetworkGrantToTxUlpData, latency);
        break;
      case TransactionType::kPushUnsolicited:
        LOG(FATAL) << "Unsolicited writes should never interact with the "
                      "solicitation window.";
    }
  }

  return absl::OkStatus();
}

}  // namespace isekai
