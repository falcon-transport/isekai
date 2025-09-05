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

#include "isekai/host/falcon/gen1/stats_manager.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/substitute.h"
#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_bitmap.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_counters.h"
#include "isekai/host/falcon/falcon_histograms.h"
#include "isekai/host/falcon/falcon_resource_credits.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"

namespace isekai {
namespace {

constexpr uint8_t kFalconXoff = 1;
constexpr uint8_t kFalconXon = 0;

// Flag: enable_xoff_timelines
constexpr std::string_view kStatVectorPacketBuilderToFalconXoff =
    "falcon.counter.xoff_packet_builder_to_falcon";
constexpr std::string_view kStatVectorRdmaToFalconXoff =
    "falcon.counter.xoff_rdma_to_falcon.host$0";

// Flag: enable_per_connection_backpressure_alpha_carving_limits
constexpr std::string_view kStatVectorAlphaCarvingRequestTxPacketLimit =
    "falcon.cid$0.alpha_carving_request_tx_packet_limit";
constexpr std::string_view kStatVectorAlphaCarvingRequestTxBufferLimit =
    "falcon.cid$0.alpha_carving_request_tx_buffer_limit";
constexpr std::string_view kStatVectorAlphaCarvingRequestRxPacketLimit =
    "falcon.cid$0.alpha_carving_request_rx_packet_limit";
constexpr std::string_view kStatVectorAlphaCarvingRequestRxBufferLimit =
    "falcon.cid$0.alpha_carving_request_rx_buffer_limit";
constexpr std::string_view kStatVectorAlphaCarvingResponseTxPacketLimit =
    "falcon.cid$0.alpha_carving_response_tx_packet_limit";
constexpr std::string_view kStatVectorAlphaCarvingResponseTxBufferLimit =
    "falcon.cid$0.alpha_carving_response_tx_buffer_limit";

// Number of unacked inflight packets in the request and window, respectively.
// Out-of-order ACKs are considered inflight. That is, window usage does not
// exclude acked non-HoL packets.
// Flag: enable_per_connection_window_usage
constexpr std::string_view kStatVectorRequestWindowUsage =
    "falcon.cid$0.counter.request_window_usage";
constexpr std::string_view kStatVectorDataWindowUsage =
    "falcon.cid$0.counter.data_window_usage";

// RSN time series corresponding to initial transmissions.
// Flag: enable_per_connection_initial_tx_rsn_timeline
constexpr std::string_view kStatVectorPullRequestInitialTxRsnSeries =
    "falcon.cid$0.counter.pull_request_inital_tx_rsn";
constexpr std::string_view kStatVectorPushUnsolicitedDataInitialTxRsnSeries =
    "falcon.cid$0.counter.push_unsolicited_data_inital_tx_rsn";

// RSN time series corresponding to when requests are received by Falcon from
// ULP.
// Flag: enable_per_connection_rx_from_ulp_rsn_timeline
constexpr std::string_view kStatVectorPullRequestRxFromUlpRsnSeries =
    "falcon.cid$0.counter.pull_request_rx_from_ulp_rsn";
constexpr std::string_view kStatVectorPushUnsolicitedDataRxFromUlpRsnSeries =
    "falcon.cid$0.counter.push_unsolicited_data_rx_from_ulp_rsn";

// RSN time series corresponding to retransmissions.
// Flag: enable_per_connection_retx_rsn_timeline
constexpr std::string_view kStatVectorPullRequestRetxRsnSeries =
    "falcon.cid$0.counter.pull_request_retx_rsn";
constexpr std::string_view kStatVectorPullRequestOooRetxRsnSeries =
    "falcon.cid$0.counter.pull_request_ooo_retx_rsn";
constexpr std::string_view kStatVectorPullRequestRackRetxRsnSeries =
    "falcon.cid$0.counter.pull_request_rack_retx_rsn";
constexpr std::string_view kStatVectorPullRequestTlpRetxRsnSeries =
    "falcon.cid$0.counter.pull_request_tlp_retx_rsn";
constexpr std::string_view kStatVectorPullRequestNackRetxRsnSeries =
    "falcon.cid$0.counter.pull_request_nack_retx_rsn";
constexpr std::string_view kStatVectorPullRequestRtoRetxRsnSeries =
    "falcon.cid$0.counter.pull_request_rto_retx_rsn";
constexpr std::string_view kStatVectorPullRequestUlpRtoRetxRsnSeries =
    "falcon.cid$0.counter.pull_request_ulp_rto_retx_rsn";
constexpr std::string_view kStatVectorPushUnsolicitedDataRetxRsnSeries =
    "falcon.cid$0.counter.push_unsolicited_data_retx_rsn";
constexpr std::string_view kStatVectorPushUnsolicitedDataOooRetxRsnSeries =
    "falcon.cid$0.counter.push_unsolicited_data_ooo_retx_rsn";
constexpr std::string_view kStatVectorPushUnsolicitedDataRackRetxRsnSeries =
    "falcon.cid$0.counter.push_unsolicited_data_rack_retx_rsn";
constexpr std::string_view kStatVectorPushUnsolicitedDataTlpRetxRsnSeries =
    "falcon.cid$0.counter.push_unsolicited_data_tlp_retx_rsn";
constexpr std::string_view kStatVectorPushUnsolicitedDataNackRetxRsnSeries =
    "falcon.cid$0.counter.push_unsolicited_data_nack_retx_rsn";
constexpr std::string_view kStatVectorPushUnsolicitedDataRtoRetxRsnSeries =
    "falcon.cid$0.counter.push_unsolicited_data_rto_retx_rsn";
constexpr std::string_view kStatVectorPushUnsolicitedDataUlpRtoRetxRsnSeries =
    "falcon.cid$0.counter.push_unsolicited_data_ulp_rto_retx_rsn";

// RSN time series corresponding to packets being accepted at the target.
// Flag: enable_per_connection_rsn_receive_timeline
constexpr std::string_view kStatVectorPullRequestRecvRsnSeries =
    "falcon.cid$0.counter.pull_request_recv_rsn";
constexpr std::string_view kStatVectorPushDataRecvRsnSeries =
    "falcon.cid$0.counter.push_data_rsn_recv_rsn";

}  // namespace

StatisticsCollectionConfig::FalconFlags
    FalconStatsManager::stats_collection_flags_;

FalconStatsManager::FalconStatsManager(FalconModelInterface* falcon)
    : falcon_(falcon), stats_collector_(falcon->get_stats_collector()) {
  if (stats_collector_ && stats_collector_->GetConfig().has_falcon_flags()) {
    stats_collection_flags_ = stats_collector_->GetConfig().falcon_flags();
  } else {
    stats_collection_flags_ = DefaultConfigGenerator::DefaultFalconStatsFlags();
  }
}

void FalconStatsManager::UpdateUlpRxCounters(Packet::Rdma::Opcode opcode,
                                             uint32_t cid) {
  if (!stats_collection_flags_.enable_per_connection_rdma_counters()) {
    return;
  }
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  switch (opcode) {
    case Packet::Rdma::Opcode::kAck:
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::kAcksFromUlp, cid),
          ++counters.acks_from_ulp);
      break;
    case Packet::Rdma::Opcode::kReadRequest:
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::kPullRequestFromUlp, cid),
          ++counters.pull_request_from_ulp);
      break;
    case Packet::Rdma::Opcode::kReadResponseOnly:
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::kPullResponseFromUlp, cid),
          ++counters.pull_response_from_ulp);
      break;
    case Packet::Rdma::Opcode::kSendOnly:
    case Packet::Rdma::Opcode::kWriteOnly:
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::kPushRequestFromUlp, cid),
          ++counters.push_request_from_ulp);
      break;
    case Packet::Rdma::Opcode::kInvalid:
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::kAcksFromUlp, cid),
          ++counters.nacks_from_ulp);
      break;
  }
}

void FalconStatsManager::UpdateUlpTxCounters(Packet::Rdma::Opcode opcode,
                                             uint32_t cid) {
  if (!stats_collection_flags_.enable_per_connection_rdma_counters()) {
    return;
  }
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  switch (opcode) {
    case Packet::Rdma::Opcode::kAck:
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::kCompletionsToUlp, cid),
          ++counters.completions_to_ulp);
      break;
    case Packet::Rdma::Opcode::kReadRequest:
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::kPullRequestToUlp, cid),
          ++counters.pull_request_to_ulp);
      break;
    case Packet::Rdma::Opcode::kReadResponseOnly:
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::kPullResponseToUlp, cid),
          ++counters.pull_response_to_ulp);
      break;
    case Packet::Rdma::Opcode::kSendOnly:
    case Packet::Rdma::Opcode::kWriteOnly:
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::kPushDataToUlp, cid),
          ++counters.push_data_to_ulp);
      break;
    case Packet::Rdma::Opcode::kInvalid:
      break;
  }
}

void FalconStatsManager::UpdateNetworkRxCounters(falcon::PacketType type,
                                                 uint32_t cid) {
  bool collect_network_counters =
      stats_collection_flags_.enable_per_connection_network_counters();
  bool collect_ack_nack_counters =
      stats_collection_flags_.enable_per_connection_ack_nack_counters();
  bool collect_initiator_counters =
      stats_collection_flags_.enable_per_connection_initiator_txn_counters();
  bool collect_target_counters =
      stats_collection_flags_.enable_per_connection_target_txn_counters();

  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  if (collect_network_counters) {
    CollectScalarStats(
        absl::Substitute(FalconConnectionCounters::kRxPackets, cid),
        ++counters.rx_packets);
  }

  switch (type) {
    case falcon::PacketType::kPullRequest:
      if (collect_target_counters) {
        CollectScalarStats(
            absl::Substitute(FalconConnectionCounters::kTargetRxPullRequest,
                             cid),
            ++counters.target_rx_pull_request);
      }
      break;
    case falcon::PacketType::kPushRequest:
      if (collect_target_counters) {
        CollectScalarStats(
            absl::Substitute(
                FalconConnectionCounters::kTargetRxPushSolicitedRequest, cid),
            ++counters.target_rx_push_solicited_request);
      }
      break;
    case falcon::PacketType::kPushGrant:
      if (collect_initiator_counters) {
        CollectScalarStats(
            absl::Substitute(FalconConnectionCounters::kInitiatorRxPushGrant,
                             cid),
            ++counters.initiator_rx_push_grant);
      }
      break;
    case falcon::PacketType::kPullData:
      if (collect_initiator_counters) {
        CollectScalarStats(
            absl::Substitute(FalconConnectionCounters::kInitiatorRxPullData,
                             cid),
            ++counters.initiator_rx_pull_data);
      }
      break;
    case falcon::PacketType::kPushSolicitedData:
      if (collect_target_counters) {
        CollectScalarStats(
            absl::Substitute(
                FalconConnectionCounters::kTargetRxPushSolicitedData, cid),
            ++counters.target_rx_push_solicited_data);
      }
      break;
    case falcon::PacketType::kPushUnsolicitedData:
      if (collect_target_counters) {
        CollectScalarStats(
            absl::Substitute(
                FalconConnectionCounters::kTargetRxPushUnsolicitedData, cid),
            ++counters.target_rx_push_unsolicited_data);
      }
      break;
    case falcon::PacketType::kNack:
      if (collect_ack_nack_counters) {
        CollectScalarStats(
            absl::Substitute(FalconConnectionCounters::kRxNacks, cid),
            ++counters.rx_nacks);
      }
      break;
    case falcon::PacketType::kAck:
      if (collect_ack_nack_counters) {
        CollectScalarStats(
            absl::Substitute(FalconConnectionCounters::kRxAcks, cid),
            ++counters.rx_acks);
      }
      break;
    default:
      break;
  }
}

void FalconStatsManager::UpdateNetworkTxCounters(falcon::PacketType type,
                                                 uint32_t cid,
                                                 bool is_retransmission,
                                                 RetransmitReason retx_reason) {
  bool collect_network_counters =
      stats_collection_flags_.enable_per_connection_network_counters();
  bool collect_ack_nack_counters =
      stats_collection_flags_.enable_per_connection_ack_nack_counters();
  bool collect_initiator_counters =
      stats_collection_flags_.enable_per_connection_initiator_txn_counters();
  bool collect_target_counters =
      stats_collection_flags_.enable_per_connection_target_txn_counters();

  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  if (collect_network_counters) {
    CollectScalarStats(
        absl::Substitute(FalconConnectionCounters::kTxPackets, cid),
        ++counters.tx_packets);
  }

  if (stats_collection_flags_.enable_per_connection_retx_counters() &&
      is_retransmission) {
    switch (retx_reason) {
      case RetransmitReason::kTimeout:
        CollectScalarStats(
            absl::Substitute(FalconConnectionCounters::kTxTimeoutRetransmitted,
                             cid),
            ++counters.tx_timeout_retransmitted);
        break;
      case RetransmitReason::kEarlyOooDis:
        CollectScalarStats(
            absl::Substitute(
                FalconConnectionCounters::kTxEarlyOooDisRetransmitted, cid),
            ++counters.tx_early_ooo_dis_retransmitted);
        break;
      case RetransmitReason::kEarlyRack:
        CollectScalarStats(
            absl::Substitute(
                FalconConnectionCounters::kTxEarlyRackRetransmitted, cid),
            ++counters.tx_early_rack_retransmitted);
        break;
      case RetransmitReason::kEarlyNack:
        CollectScalarStats(
            absl::Substitute(
                FalconConnectionCounters::kTxEarlyNackRetransmitted, cid),
            ++counters.tx_early_nack_retransmitted);
        break;
      case RetransmitReason::kEarlyTlp:
        CollectScalarStats(
            absl::Substitute(FalconConnectionCounters::kTxEarlyTlpRetransmitted,
                             cid),
            ++counters.tx_early_tlp_retransmitted);
        break;
      case RetransmitReason::kUlpRto:
        CollectScalarStats(
            absl::Substitute(FalconConnectionCounters::kTxUlpRetransmitted,
                             cid),
            ++counters.tx_ulp_retransmitted);
        break;
    }
  }
  switch (type) {
    case falcon::PacketType::kPullRequest:
      if (collect_initiator_counters) {
        CollectScalarStats(
            absl::Substitute(FalconConnectionCounters::kInitiatorTxPullRequest,
                             cid),
            ++counters.initiator_tx_pull_request);
      }
      break;
    case falcon::PacketType::kPushRequest:
      if (collect_initiator_counters) {
        CollectScalarStats(
            absl::Substitute(
                FalconConnectionCounters::kInitiatorTxPushSolicitedRequest,
                cid),
            ++counters.initiator_tx_push_solicited_request);
      }
      break;
    case falcon::PacketType::kPushGrant:
      if (collect_target_counters) {
        CollectScalarStats(
            absl::Substitute(FalconConnectionCounters::kTargetTxPushGrant, cid),
            ++counters.target_tx_push_grant);
      }
      break;
    case falcon::PacketType::kPullData:
      if (collect_target_counters) {
        CollectScalarStats(
            absl::Substitute(FalconConnectionCounters::kTargetTxPullData, cid),
            ++counters.target_tx_pull_data);
      }
      break;
    case falcon::PacketType::kPushSolicitedData:
      if (collect_initiator_counters) {
        CollectScalarStats(
            absl::Substitute(
                FalconConnectionCounters::kInitiatorTxPushSolicitedData, cid),
            ++counters.initiator_tx_push_solicited_data);
      }
      break;
    case falcon::PacketType::kPushUnsolicitedData:
      if (collect_initiator_counters) {
        CollectScalarStats(
            absl::Substitute(
                FalconConnectionCounters::kInitiatorTxPushUnsolicitedData, cid),
            ++counters.initiator_tx_push_unsolicited_data);
      }
      break;
    case falcon::PacketType::kNack:
      if (collect_ack_nack_counters) {
        CollectScalarStats(
            absl::Substitute(FalconConnectionCounters::kTxNacks, cid),
            ++counters.tx_nacks);
      }
      break;
    case falcon::PacketType::kAck:
      if (collect_ack_nack_counters) {
        CollectScalarStats(
            absl::Substitute(FalconConnectionCounters::kTxAcks, cid),
            ++counters.tx_acks);
      }
      break;
    default:
      break;
  }
}

void FalconStatsManager::UpdateMaxTransmissionCount(uint32_t attempts) {
  if (stats_collection_flags_.enable_max_retransmissions() &&
      attempts > host_counters_.max_transmission_attempts) {
    host_counters_.max_transmission_attempts = attempts;
    CollectScalarStats(FalconHostCounters::kStatScalarMaxTransmissionAttempts,
                       host_counters_.max_transmission_attempts);
  }
}

void FalconStatsManager::UpdateRueEventCounters(uint32_t cid,
                                                falcon::RueEventType event,
                                                bool eack, bool eack_drop) {
  if (!stats_collection_flags_.enable_per_connection_rue_counters()) {
    return;
  }
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  switch (event) {
    case falcon::RueEventType::kAck:
      if (eack) {
        if (eack_drop) {
          CollectScalarStats(
              absl::Substitute(FalconConnectionCounters::kRueEackDropEvents,
                               cid),
              ++counters.rue_eack_drop_events);
        } else {
          CollectScalarStats(
              absl::Substitute(FalconConnectionCounters::kRueEackEvents, cid),
              ++counters.rue_eack_events);
        }
      } else {
        CollectScalarStats(
            absl::Substitute(FalconConnectionCounters::kRueAckEvents, cid),
            ++counters.rue_ack_events);
      }
      break;
    case falcon::RueEventType::kNack:
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::kRueNackEvents, cid),
          ++counters.rue_nack_events);
      break;
    case falcon::RueEventType::kRetransmit:
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::kRueRetransmitEvents, cid),
          ++counters.rue_retransmit_events);
      break;
  }
}

void FalconStatsManager::UpdateRueResponseCounters(uint32_t cid) {
  if (!stats_collection_flags_.enable_per_connection_rue_counters()) {
    return;
  }
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  CollectVectorStats(
      absl::Substitute(FalconConnectionCounters::kRueResponses, cid),
      ++counters.rue_responses);
}

void FalconStatsManager::UpdateRueEnqueueAttempts(uint32_t cid) {
  if (!stats_collection_flags_.enable_per_connection_rue_drop_counters()) {
    return;
  }
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  CollectVectorStats(
      absl::Substitute(FalconConnectionCounters::kRueEnqueueAttempts, cid),
      ++counters.rue_enqueue_attempts);
}

void FalconStatsManager::UpdateRueDroppedEventCounters(
    uint32_t cid, falcon::RueEventType event, bool eack, bool eack_drop) {
  if (!stats_collection_flags_.enable_per_connection_rue_drop_counters()) {
    return;
  }
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  switch (event) {
    case falcon::RueEventType::kAck:
      if (eack) {
        if (eack_drop) {
          CollectVectorStats(
              absl::Substitute(
                  FalconConnectionCounters::kRueEventDroppedEackDrops, cid),
              ++counters.rue_event_drops_eack_drop);
        } else {
          CollectVectorStats(
              absl::Substitute(FalconConnectionCounters::kRueEventDroppedEacks,
                               cid),
              ++counters.rue_event_drops_eack);
        }
      } else {
        CollectVectorStats(
            absl::Substitute(FalconConnectionCounters::kRueEventDroppedAcks,
                             cid),
            ++counters.rue_event_drops_ack);
      }
      break;
    case falcon::RueEventType::kNack:
      CollectVectorStats(
          absl::Substitute(FalconConnectionCounters::kRueEventDroppedNacks,
                           cid),
          ++counters.rue_event_drops_nack);
      break;
    case falcon::RueEventType::kRetransmit:
      CollectVectorStats(
          absl::Substitute(
              FalconConnectionCounters::kRueEventDroppedRetransmits, cid),
          ++counters.rue_event_drops_retransmit);
      break;
  }
}

void FalconStatsManager::UpdateAcksGeneratedCounterDueToAR(uint32_t cid) {
  if (!stats_collection_flags_.enable_per_connection_ack_reason_counters()) {
    return;
  }
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  CollectVectorStats(
      absl::Substitute(FalconConnectionCounters::kAcksGeneratedDueToAR, cid),
      ++counters.acks_generated_due_to_ar);
}

void FalconStatsManager::UpdateAcksGeneratedCounterDueToTimeout(uint32_t cid) {
  if (!stats_collection_flags_.enable_per_connection_ack_reason_counters()) {
    return;
  }
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  CollectVectorStats(
      absl::Substitute(FalconConnectionCounters::kAcksGeneratedDueToTimeout,
                       cid),
      ++counters.acks_generated_due_to_timeout);
}

void FalconStatsManager::UpdateAcksGeneratedCounterDueToCoalescingCounter(
    uint32_t cid) {
  if (!stats_collection_flags_.enable_per_connection_ack_reason_counters()) {
    return;
  }
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  CollectVectorStats(
      absl::Substitute(
          FalconConnectionCounters::kAcksGeneratedDueToCoalescingCounter, cid),
      ++counters.acks_generated_due_to_coalescing_counter);
}

void FalconStatsManager::UpdateNetworkRxDropCounters(falcon::PacketType type,
                                                     uint32_t cid,
                                                     absl::Status drop_reason) {
  if (!stats_collection_flags_.enable_per_connection_packet_drop_counters()) {
    return;
  }
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  CollectScalarStats(
      absl::Substitute(FalconConnectionCounters::kRxDroppedPackets, cid),
      ++counters.total_rx_dropped_pkts);

  if (IsFalconTransaction(type)) {
    if (drop_reason.code() == absl::StatusCode::kResourceExhausted) {
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::kRxDroppedRsrcPackets,
                           cid),
          ++counters.rx_resource_dropped_transaction_pkts);
    } else if (drop_reason.code() == absl::StatusCode::kOutOfRange) {
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::kRxDroppedWindowPackets,
                           cid),
          ++counters.rx_window_dropped_transaction_pkts);
    } else if (drop_reason.code() == absl::StatusCode::kAlreadyExists) {
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::kRxDroppedDuplicatePackets,
                           cid),
          ++counters.rx_duplicate_dropped_transaction_pkts);
    } else {
      LOG(FATAL) << "Encountered an invalid drop cause for the transaction.";
    }
  } else if (type == falcon::PacketType::kAck) {
    if (drop_reason.code() == absl::StatusCode::kAlreadyExists) {
      CollectScalarStats(
          absl::Substitute(
              FalconConnectionCounters::kRxDroppedDuplicateAckPackets, cid),
          ++counters.rx_duplicate_dropped_ack_pkts);
    } else if (drop_reason.code() == absl::StatusCode::kFailedPrecondition) {
      CollectScalarStats(
          absl::Substitute(FalconConnectionCounters::
                               kRxDroppedStaleAckPacketsWithZeroNumAcked,
                           cid),
          ++counters.rx_stale_dropped_ack_pkts_with_zero_num_acked);
    } else {
      LOG(FATAL) << "Encountered an invalid drop cause for the ACK: "
                 << drop_reason;
    }
  } else if (type == falcon::PacketType::kNack) {
    if (drop_reason.code() == absl::StatusCode::kAlreadyExists) {
      CollectScalarStats(
          absl::Substitute(
              FalconConnectionCounters::kRxDroppedDuplicateNackPackets, cid),
          ++counters.rx_duplicate_dropped_nack_pkts);
    } else {
      LOG(FATAL) << "Encountered an invalid drop cause for the NACK: "
                 << drop_reason;
    }
  } else {
    LOG(FATAL) << "Unexpected packet dropped.";
  }
}

void FalconStatsManager::UpdateSolicitationCounters(uint32_t cid,
                                                    uint64_t window_bytes,
                                                    bool is_release) {
  if (!stats_collection_flags_.enable_solicitation_counters()) {
    return;
  }
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  if (is_release) {
    counters.solicitation_window_occupied_bytes -= window_bytes;
    CHECK_GE(counters.solicitation_window_occupied_bytes, 0);
  } else {
    counters.solicitation_window_occupied_bytes += window_bytes;
  }
  CollectVectorStats(
      absl::Substitute(
          FalconConnectionCounters::kSolicitationWindowOccupiedBytes, cid),
      counters.solicitation_window_occupied_bytes);
}

void FalconStatsManager::UpdateRequestOrDataWindowUsage(
    WindowType type, uint32_t cid, uint64_t occupied_bytes) {
  if (stats_collection_flags_.enable_per_connection_window_usage()) {
    CollectVectorStats(absl::Substitute(type == WindowType::kRequest
                                            ? kStatVectorRequestWindowUsage
                                            : kStatVectorDataWindowUsage,
                                        cid),
                       occupied_bytes);
  }
}

void FalconStatsManager::UpdateAlphaCarvingDynamicLimitCounters(
    uint32_t cid, const FalconCredit& credit_limits) {
  if (!stats_collection_flags_
           .enable_per_connection_backpressure_alpha_carving_limits()) {
    return;
  }
  CollectVectorStats(
      absl::Substitute(kStatVectorAlphaCarvingRequestTxPacketLimit, cid),
      credit_limits.request_tx_packet);
  CollectVectorStats(
      absl::Substitute(kStatVectorAlphaCarvingRequestTxBufferLimit, cid),
      credit_limits.request_tx_buffer);
  CollectVectorStats(
      absl::Substitute(kStatVectorAlphaCarvingRequestRxPacketLimit, cid),
      credit_limits.request_rx_packet);
  CollectVectorStats(
      absl::Substitute(kStatVectorAlphaCarvingRequestRxBufferLimit, cid),
      credit_limits.request_rx_buffer);
  CollectVectorStats(
      absl::Substitute(kStatVectorAlphaCarvingResponseTxPacketLimit, cid),
      credit_limits.response_tx_packet);
  CollectVectorStats(
      absl::Substitute(kStatVectorAlphaCarvingResponseTxBufferLimit, cid),
      credit_limits.response_tx_buffer);
}

void FalconStatsManager::UpdateResourceCounters(uint32_t cid,
                                                FalconResourceCredits credit,
                                                bool is_release) {
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  if (is_release) {
    counters.tx_pkt_credits_ulp_requests -=
        credit.tx_packet_credits.ulp_requests;
    counters.tx_pkt_credits_ulp_data -= credit.tx_packet_credits.ulp_data;
    counters.tx_pkt_credits_network_requests -=
        credit.tx_packet_credits.network_requests;
    counters.tx_buf_credits_ulp_requests -=
        credit.tx_buffer_credits.ulp_requests;
    counters.tx_buf_credits_ulp_data -= credit.tx_buffer_credits.ulp_data;

    counters.tx_buf_credits_network_requests -=
        credit.tx_buffer_credits.network_requests;
    counters.rx_pkt_credits_ulp_requests -=
        credit.rx_packet_credits.ulp_requests;
    counters.rx_pkt_credits_network_requests -=
        credit.rx_packet_credits.network_requests;
    counters.rx_buf_credits_ulp_requests -=
        credit.rx_buffer_credits.ulp_requests;
    counters.rx_buf_credits_network_requests -=
        credit.rx_buffer_credits.network_requests;
  } else {
    counters.tx_pkt_credits_ulp_requests +=
        credit.tx_packet_credits.ulp_requests;
    counters.tx_pkt_credits_ulp_data += credit.tx_packet_credits.ulp_data;
    counters.tx_pkt_credits_network_requests +=
        credit.tx_packet_credits.network_requests;
    counters.tx_buf_credits_ulp_requests +=
        credit.tx_buffer_credits.ulp_requests;
    counters.tx_buf_credits_ulp_data += credit.tx_buffer_credits.ulp_data;

    counters.tx_buf_credits_network_requests +=
        credit.tx_buffer_credits.network_requests;
    counters.rx_pkt_credits_ulp_requests +=
        credit.rx_packet_credits.ulp_requests;
    counters.rx_pkt_credits_network_requests +=
        credit.rx_packet_credits.network_requests;
    counters.rx_buf_credits_ulp_requests +=
        credit.rx_buffer_credits.ulp_requests;
    counters.rx_buf_credits_network_requests +=
        credit.rx_buffer_credits.network_requests;
  }

  if (!stats_collection_flags_
           .enable_per_connection_resource_credit_counters()) {
    return;
  }

  CollectVectorStats(
      absl::Substitute(FalconConnectionCounters::kPerConnTxPktCreditsUlpReqs,
                       cid),
      counters.tx_pkt_credits_ulp_requests);
  CollectVectorStats(
      absl::Substitute(FalconConnectionCounters::kPerConnTxPktCreditsUlpData,
                       cid),
      counters.tx_pkt_credits_ulp_data);
  CollectVectorStats(
      absl::Substitute(FalconConnectionCounters::kPerConnTxPktCreditsNtwkReqs,
                       cid),
      counters.tx_pkt_credits_network_requests);
  CollectVectorStats(
      absl::Substitute(FalconConnectionCounters::kPerConnTxBufferCreditsUlpReqs,
                       cid),
      counters.tx_buf_credits_ulp_requests);
  CollectVectorStats(
      absl::Substitute(FalconConnectionCounters::kPerConnTxBufferCreditsUlpData,
                       cid),
      counters.tx_buf_credits_ulp_data);
  CollectVectorStats(
      absl::Substitute(
          FalconConnectionCounters::kPerConnTxBufferCreditsNtwkReqs, cid),
      counters.tx_buf_credits_network_requests);
  CollectVectorStats(
      absl::Substitute(FalconConnectionCounters::kPerConnRxPktCreditsUlpReqs,
                       cid),
      counters.rx_pkt_credits_ulp_requests);
  CollectVectorStats(
      absl::Substitute(FalconConnectionCounters::kPerConnRxPktCreditsNtwkReqs,
                       cid),
      counters.rx_pkt_credits_network_requests);
  CollectVectorStats(
      absl::Substitute(FalconConnectionCounters::kPerConnRxBufferCreditsUlpReqs,
                       cid),
      counters.rx_buf_credits_ulp_requests);
  CollectVectorStats(
      absl::Substitute(
          FalconConnectionCounters::kPerConnRxBufferCreditsNtwkReqs, cid),
      counters.rx_buf_credits_network_requests);
}

void FalconStatsManager::UpdateSchedulerCounters(SchedulerTypes scheduler_type,
                                                 bool is_dequed) {
  if (!stats_collection_flags_.enable_vector_scheduler_lengths() &&
      !stats_collection_flags_.enable_histogram_scheduler_lengths()) {
    return;
  }
  switch (scheduler_type) {
    case SchedulerTypes::kConnection:
      if (is_dequed)
        host_counters_.connection_scheduler_packets -= 1;
      else
        host_counters_.connection_scheduler_packets += 1;
      break;
    case SchedulerTypes::kRetransmission:
      if (is_dequed)
        host_counters_.retransmission_scheduler_packets -= 1;
      else
        host_counters_.retransmission_scheduler_packets += 1;
      break;
    case SchedulerTypes::kAckNack:
      if (is_dequed)
        host_counters_.ack_scheduler_packets -= 1;
      else
        host_counters_.ack_scheduler_packets += 1;
      break;
  }

  if (!is_dequed) {
    return;
  }

  if (stats_collection_flags_.enable_vector_scheduler_lengths()) {
    CollectVectorStats(
        FalconHostCounters::kConnectionSchedulerOutstandingPacketCount,
        host_counters_.connection_scheduler_packets);
    CollectVectorStats(
        FalconHostCounters::kRetransmissionSchedulerOutstandingPacketCount,
        host_counters_.retransmission_scheduler_packets);
    CollectVectorStats(FalconHostCounters::kAckSchedulerOutstandingPacketCount,
                       host_counters_.ack_scheduler_packets);
  }

  if (stats_collection_flags_.enable_histogram_scheduler_lengths()) {
    histogram_collector_.Add(
        SchedulerTypes::kConnection,
        static_cast<double>(host_counters_.connection_scheduler_packets));
    histogram_collector_.Add(
        SchedulerTypes::kRetransmission,
        static_cast<double>(host_counters_.retransmission_scheduler_packets));
    histogram_collector_.Add(
        SchedulerTypes::kAckNack,
        static_cast<double>(host_counters_.ack_scheduler_packets));
  }
}

void FalconStatsManager::UpdateIntraConnectionSchedulerCounters(
    uint32_t cid, PacketTypeQueue queue_type, bool is_dequed) {
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  switch (queue_type) {
    case PacketTypeQueue::kPullAndOrderedPushRequest:
      if (is_dequed)
        --counters.pull_and_ordered_push_request_queue_packets;
      else
        ++counters.pull_and_ordered_push_request_queue_packets;
      break;
    case PacketTypeQueue::kUnorderedPushRequest:
      if (is_dequed)
        --counters.unordered_push_request_queue_packets;
      else
        ++counters.unordered_push_request_queue_packets;
      break;
    case PacketTypeQueue::kPushData:
      if (is_dequed)
        --counters.push_data_queue_packets;
      else
        ++counters.push_data_queue_packets;
      break;
    case PacketTypeQueue::kPushGrant:
      if (is_dequed)
        --counters.push_grant_queue_packets;
      else
        ++counters.push_grant_queue_packets;
      break;
    case PacketTypeQueue::kPullData:
      if (is_dequed)
        --counters.pull_data_queue_packets;
      else
        ++counters.pull_data_queue_packets;
      break;
  }

  if (stats_collection_flags_.enable_per_connection_scheduler_queue_length()) {
    CollectVectorStats(
        absl::Substitute(
            FalconConnectionCounters::kPullAndOrderedPushRequestQueueCounter,
            cid),
        counters.pull_and_ordered_push_request_queue_packets);
    CollectVectorStats(
        absl::Substitute(
            FalconConnectionCounters::kUnorderedPushRequestQueueCounter, cid),
        counters.unordered_push_request_queue_packets);
    CollectVectorStats(
        absl::Substitute(FalconConnectionCounters::kPushDataQueueCounter, cid),
        counters.push_data_queue_packets);
    CollectVectorStats(
        absl::Substitute(FalconConnectionCounters::kPushGrantQueueCounter, cid),
        counters.push_grant_queue_packets);
    CollectVectorStats(
        absl::Substitute(FalconConnectionCounters::kPullDataQueueCounter, cid),
        counters.pull_data_queue_packets);
  }

  if (stats_collection_flags_
          .enable_per_connection_scheduler_queue_length_histogram() &&
      is_dequed) {
    histogram_collector_.Add(cid, counters);
  }
}

void FalconStatsManager::UpdateCwndPauseCounters(uint32_t cid, bool is_paused) {
  if (!stats_collection_flags_.enable_per_connection_cwnd_pause()) {
    return;
  }
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  double pause_time = 0;
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       falcon_->get_state_manager()->PerformDirectLookup(cid));
  if (is_paused) {
    // When a cwnd pause starts, update the cwnd_pause_start_time metadata.
    if (connection_state->cwnd_pause_start_time == absl::ZeroDuration()) {
      connection_state->cwnd_pause_start_time =
          falcon_->get_environment()->ElapsedTime();
    }
    return;
  } else {
    // When a cwnd pause ends, update the cwnd pause counter.
    if (connection_state->cwnd_pause_start_time != absl::ZeroDuration()) {
      absl::Duration now = falcon_->get_environment()->ElapsedTime();
      pause_time = absl::ToDoubleNanoseconds(
          now - connection_state->cwnd_pause_start_time);
      connection_state->cwnd_pause_start_time = absl::ZeroDuration();
    }
  }
  counters.connection_cwnd_pause_time += pause_time;
  CollectScalarStats(
      absl::Substitute(FalconConnectionCounters::kPerConnectionCwndPauseTime,
                       cid),
      counters.connection_cwnd_pause_time);
}

void FalconStatsManager::UpdateInitialTxRsnSeries(uint32_t cid, uint32_t rsn,
                                                  falcon::PacketType type) {
  if (stats_collection_flags_.enable_per_connection_initial_tx_rsn_timeline()) {
    switch (type) {
      case falcon::PacketType::kPullRequest:
        CollectVectorStats(
            absl::Substitute(kStatVectorPullRequestInitialTxRsnSeries, cid),
            rsn);
        break;
      case falcon::PacketType::kPushUnsolicitedData:
        CollectVectorStats(
            absl::Substitute(kStatVectorPushUnsolicitedDataInitialTxRsnSeries,
                             cid),
            rsn);
        break;
      default:
        break;
    }
  }
}

void FalconStatsManager::UpdateRxFromUlpRsnSeries(uint32_t cid, uint32_t rsn,
                                                  falcon::PacketType type) {
  if (stats_collection_flags_
          .enable_per_connection_rx_from_ulp_rsn_timeline()) {
    switch (type) {
      case falcon::PacketType::kPullRequest:
        CollectVectorStats(
            absl::Substitute(kStatVectorPullRequestRxFromUlpRsnSeries, cid),
            rsn);
        break;
      case falcon::PacketType::kPushUnsolicitedData:
        CollectVectorStats(
            absl::Substitute(kStatVectorPushUnsolicitedDataRxFromUlpRsnSeries,
                             cid),
            rsn);
        break;
      default:
        break;
    }
  }
}

void FalconStatsManager::UpdateRetxRsnSeries(uint32_t cid, uint32_t rsn,
                                             falcon::PacketType type,
                                             RetransmitReason retx_reason) {
  if (!stats_collection_flags_.enable_per_connection_retx_rsn_timeline()) {
    return;
  }
  if (type == falcon::PacketType::kPullRequest) {
    CollectVectorStats(
        absl::Substitute(kStatVectorPullRequestRetxRsnSeries, cid), rsn);
    switch (retx_reason) {
      case RetransmitReason::kTimeout:
        CollectVectorStats(
            absl::Substitute(kStatVectorPullRequestRtoRetxRsnSeries, cid), rsn);
        break;
      case RetransmitReason::kUlpRto:
        CollectVectorStats(
            absl::Substitute(kStatVectorPullRequestUlpRtoRetxRsnSeries, cid),
            rsn);
        break;
      case RetransmitReason::kEarlyOooDis:
        CollectVectorStats(
            absl::Substitute(kStatVectorPullRequestOooRetxRsnSeries, cid), rsn);
        break;
      case RetransmitReason::kEarlyRack:
        CollectVectorStats(
            absl::Substitute(kStatVectorPullRequestRackRetxRsnSeries, cid),
            rsn);
        break;
      case RetransmitReason::kEarlyTlp:
        CollectVectorStats(
            absl::Substitute(kStatVectorPullRequestTlpRetxRsnSeries, cid), rsn);
        break;
      case RetransmitReason::kEarlyNack:
        CollectVectorStats(
            absl::Substitute(kStatVectorPullRequestNackRetxRsnSeries, cid),
            rsn);
        break;
    }
  } else if (type == falcon::PacketType::kPushUnsolicitedData) {
    CollectVectorStats(
        absl::Substitute(kStatVectorPushUnsolicitedDataRetxRsnSeries, cid),
        rsn);
    switch (retx_reason) {
      case RetransmitReason::kTimeout:
        CollectVectorStats(
            absl::Substitute(kStatVectorPushUnsolicitedDataRtoRetxRsnSeries,
                             cid),
            rsn);
        break;
      case RetransmitReason::kUlpRto:
        CollectVectorStats(
            absl::Substitute(kStatVectorPushUnsolicitedDataUlpRtoRetxRsnSeries,
                             cid),
            rsn);
        break;
      case RetransmitReason::kEarlyOooDis:
        CollectVectorStats(
            absl::Substitute(kStatVectorPushUnsolicitedDataOooRetxRsnSeries,
                             cid),
            rsn);
        break;
      case RetransmitReason::kEarlyRack:
        CollectVectorStats(
            absl::Substitute(kStatVectorPushUnsolicitedDataRackRetxRsnSeries,
                             cid),
            rsn);
        break;
      case RetransmitReason::kEarlyTlp:
        CollectVectorStats(
            absl::Substitute(kStatVectorPushUnsolicitedDataTlpRetxRsnSeries,
                             cid),
            rsn);
        break;
      case RetransmitReason::kEarlyNack:
        CollectVectorStats(
            absl::Substitute(kStatVectorPushUnsolicitedDataNackRetxRsnSeries,
                             cid),
            rsn);
        break;
    }
  }
}

void FalconStatsManager::UpdateNetworkAcceptedRsnSeries(
    uint32_t cid, uint32_t accepted_rsn, falcon::PacketType type) {
  if (stats_collection_flags_.enable_per_connection_rsn_receive_timeline()) {
    switch (type) {
      case falcon::PacketType::kPullRequest:
        CollectVectorStats(
            absl::Substitute(kStatVectorPullRequestRecvRsnSeries, cid),
            accepted_rsn);
        break;
      case falcon::PacketType::kPushUnsolicitedData:
        CollectVectorStats(
            absl::Substitute(kStatVectorPushDataRecvRsnSeries, cid),
            accepted_rsn);
        break;
      default:
        break;
    }
  }
}

void FalconStatsManager::UpdateMaxRsnDistance(uint32_t cid,
                                              uint32_t rsn_difference) {
  if (!stats_collection_flags_.enable_per_connection_max_rsn_difference()) {
    return;
  }
  FalconConnectionCounters& counters = GetConnectionCounters(cid);
  if (rsn_difference > counters.max_pull_push_unsolicited_rsn_distance) {
    counters.max_pull_push_unsolicited_rsn_distance = rsn_difference;
    CollectScalarStats(
        absl::Substitute(FalconConnectionCounters::kMaxPullPushRsnDistance,
                         cid),
        counters.max_pull_push_unsolicited_rsn_distance);
  }
}

void FalconStatsManager::UpdatePacketBuilderXoff(bool xoff) {
  if (stats_collection_flags_.enable_xoff_timelines()) {
    if (xoff) {
      CollectVectorStats(kStatVectorPacketBuilderToFalconXoff, kFalconXoff);
    } else {
      CollectVectorStats(kStatVectorPacketBuilderToFalconXoff, kFalconXon);
    }
  }
}

void FalconStatsManager::UpdateRdmaXoff(uint8_t bifurcation_id, bool xoff) {
  if (stats_collection_flags_.enable_xoff_timelines()) {
    if (xoff) {
      CollectVectorStats(
          absl::Substitute(kStatVectorRdmaToFalconXoff,
                           static_cast<uint32_t>(bifurcation_id)),
          kFalconXoff);
    } else {
      CollectVectorStats(
          absl::Substitute(kStatVectorRdmaToFalconXoff,
                           static_cast<uint32_t>(bifurcation_id)),
          kFalconXon);
    }
  }
}

void FalconStatsManager::UpdatePacketBuilderRxBytes(uint32_t cid,
                                                    uint32_t pkt_size_bytes) {
  if (stats_collection_flags_.enable_per_connection_network_counters()) {
    FalconConnectionCounters& counters = GetConnectionCounters(cid);
    counters.rx_bytes += pkt_size_bytes;
    CollectVectorStats(
        absl::Substitute(FalconConnectionCounters::kRxBytes, cid),
        counters.rx_bytes);
  }
}

void FalconStatsManager::UpdatePacketBuilderTxBytes(uint32_t cid,
                                                    uint32_t pkt_size_bytes) {
  if (stats_collection_flags_.enable_per_connection_network_counters()) {
    FalconConnectionCounters& counters = GetConnectionCounters(cid);
    counters.tx_bytes += pkt_size_bytes;
    CollectVectorStats(
        absl::Substitute(FalconConnectionCounters::kTxBytes, cid),
        counters.tx_bytes);
  }
}

void FalconStatsManager::CollectScalarStats(const std::string_view stat_name,
                                            double value) {
  if (stats_collector_) {
    CHECK_OK(stats_collector_->UpdateStatistic(
        std::string(stat_name), value,
        StatisticsCollectionConfig::SCALAR_MAX_STAT));
  }
}

void FalconStatsManager::CollectVectorStats(const std::string_view stat_name,
                                            double value) {
  if (stats_collector_) {
    CHECK_OK(stats_collector_->UpdateStatistic(
        std::string(stat_name), value,
        StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
}

}  // namespace isekai
