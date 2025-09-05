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

#ifndef ISEKAI_HOST_FALCON_FALCON_COUNTERS_H_
#define ISEKAI_HOST_FALCON_FALCON_COUNTERS_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace isekai {

struct FalconHostCounters {
  // Flags: enable_vector_scheduler_lengths, enable_histogram_scheduler_lengths
  static constexpr std::string_view kConnectionSchedulerOutstandingPacketCount =
      "falcon.counter.connection_scheduler_outstanding_packets";
  static constexpr std::string_view
      kRetransmissionSchedulerOutstandingPacketCount =
          "falcon.counter.retransmission_scheduler_outstanding_packets";
  static constexpr std::string_view kAckSchedulerOutstandingPacketCount =
      "falcon.counter.ack_scheduler_outstanding_packets";
  uint32_t connection_scheduler_packets = 0;
  uint32_t retransmission_scheduler_packets = 0;
  uint32_t ack_scheduler_packets = 0;

  // Flags: enable_max_retransmissions
  static constexpr std::string_view kStatScalarMaxTransmissionAttempts =
      "falcon.counter.max_transmission_attempts";
  uint32_t max_transmission_attempts = 0;
};

struct FalconConnectionCounters {
  // RDMA facing counters.
  // Flag: enable_per_connection_rdma_counters
  static constexpr std::string_view kPullRequestFromUlp =
      "falcon.cid$0.counter.pull_request_from_ulp";
  static constexpr std::string_view kPushRequestFromUlp =
      "falcon.cid$0.counter.push_request_from_ulp";
  static constexpr std::string_view kPullResponseToUlp =
      "falcon.cid$0.counter.pull_response_to_ulp";
  static constexpr std::string_view kCompletionsToUlp =
      "falcon.cid$0.counter.completions_to_ulp";
  static constexpr std::string_view kPullRequestToUlp =
      "falcon.cid$0.counter.pull_request_to_ulp";
  static constexpr std::string_view kPushDataToUlp =
      "falcon.cid$0.counter.push_data_to_ulp";
  static constexpr std::string_view kPullResponseFromUlp =
      "falcon.cid$0.counter.pull_response_from_ulp";
  static constexpr std::string_view kAcksFromUlp =
      "falcon.cid$0.counter.acks_from_ulp";
  static constexpr std::string_view kNacksFromUlp =
      "falcon.cid$0.counter.nacks_from_ulp";
  // As initiator.
  uint32_t pull_request_from_ulp = 0;
  uint32_t push_request_from_ulp = 0;
  uint32_t pull_response_to_ulp = 0;
  uint32_t completions_to_ulp = 0;
  // As target.
  uint32_t pull_request_to_ulp = 0;
  uint32_t push_data_to_ulp = 0;
  uint32_t pull_response_from_ulp = 0;
  uint32_t acks_from_ulp = 0;
  uint32_t nacks_from_ulp = 0;

  // Top-level network counters.
  // Flag: enable_per_connection_network_counters
  static constexpr std::string_view kRxPackets =
      "falcon.cid$0.counter.rx_packets";
  static constexpr std::string_view kTxPackets =
      "falcon.cid$0.counter.tx_packets";
  // Updated by the packet builder (and includes all header overheads).
  static constexpr std::string_view kTxBytes = "falcon.cid$0.counter.tx_bytes";
  static constexpr std::string_view kRxBytes = "falcon.cid$0.counter.rx_bytes";
  uint32_t rx_packets = 0;
  uint32_t tx_packets = 0;
  uint64_t rx_bytes = 0;
  uint64_t tx_bytes = 0;

  // Ack/Nack counters.
  // Flag: enable_per_connection_ack_nack_counters
  static constexpr std::string_view kRxAcks = "falcon.cid$0.counter.rx_acks";
  static constexpr std::string_view kTxAcks = "falcon.cid$0.counter.tx_acks";
  static constexpr std::string_view kRxNacks = "falcon.cid$0.counter.rx_nacks";
  static constexpr std::string_view kTxNacks = "falcon.cid$0.counter.tx_nacks";
  uint32_t rx_acks = 0;
  uint32_t tx_acks = 0;
  uint32_t rx_nacks = 0;
  uint32_t tx_nacks = 0;

  // Initiator side transaction counters.
  // Flag: enable_per_connection_initiator_txn_counters
  static constexpr std::string_view kInitiatorTxPushSolicitedRequest =
      "falcon.cid$0.counter.initiator_tx_push_solicited_request";
  static constexpr std::string_view kInitiatorTxPushUnsolicitedData =
      "falcon.cid$0.counter.initiator_tx_push_unsolicited_data";
  static constexpr std::string_view kInitiatorTxPullRequest =
      "falcon.cid$0.counter.initiator_tx_pull_request";
  static constexpr std::string_view kInitiatorTxPushSolicitedData =
      "falcon.cid$0.counter.initiator_tx_push_solicited_data";
  static constexpr std::string_view kInitiatorRxPushGrant =
      "falcon.cid$0.counter.initiator_rx_push_grant";
  static constexpr std::string_view kInitiatorRxPullData =
      "falcon.cid$0.counter.initiator_rx_pull_data";
  uint32_t initiator_tx_push_solicited_request = 0;
  uint32_t initiator_tx_push_unsolicited_data = 0;
  uint32_t initiator_tx_pull_request = 0;
  uint32_t initiator_tx_push_solicited_data = 0;
  uint32_t initiator_rx_push_grant = 0;
  uint32_t initiator_rx_pull_data = 0;

  // Target side transaction counters.
  // Flag: enable_per_connection_target_txn_counters
  static constexpr std::string_view kTargetTxPullData =
      "falcon.cid$0.counter.target_tx_pull_data";
  static constexpr std::string_view kTargetTxPushGrant =
      "falcon.cid$0.counter.target_tx_push_grant";
  static constexpr std::string_view kTargetRxPushSolicitedRequest =
      "falcon.cid$0.counter.target_rx_push_solicited_request";
  static constexpr std::string_view kTargetRxPushSolicitedData =
      "falcon.cid$0.counter.target_rx_push_solicited_data";
  static constexpr std::string_view kTargetRxPushUnsolicitedData =
      "falcon.cid$0.counter.target_rx_push_unsolicited_data";
  static constexpr std::string_view kTargetRxPullRequest =
      "falcon.cid$0.counter.target_rx_pull_request";
  uint32_t target_tx_pull_data = 0;
  uint32_t target_tx_push_grant = 0;
  uint32_t target_rx_push_solicited_request = 0;
  uint32_t target_rx_push_solicited_data = 0;
  uint32_t target_rx_push_unsolicited_data = 0;
  uint32_t target_rx_pull_request = 0;

  // RUE Event and Response counters.
  // Flag: enable_per_connection_rue_counters
  static constexpr std::string_view kRueAckEvents =
      "falcon.cid$0.counter.rue_ack_events";
  static constexpr std::string_view kRueNackEvents =
      "falcon.cid$0.counter.rue_nack_events";
  static constexpr std::string_view kRueRetransmitEvents =
      "falcon.cid$0.counter.rue_retransmit_events";
  static constexpr std::string_view kRueEackEvents =
      "falcon.cid$0.counter.rue_eack_events";
  static constexpr std::string_view kRueEackDropEvents =
      "falcon.cid$0.counter.rue_eack_drop_events";
  static constexpr std::string_view kRueResponses =
      "falcon.cid$0.counter.rue_responses";
  uint32_t rue_ack_events = 0;         // Ack events enqueued to the RUE.
  uint32_t rue_nack_events = 0;        // Nack events enqueued to the RUE.
  uint32_t rue_retransmit_events = 0;  // Retransmit events enqueued to the RUE.
  uint32_t rue_eack_events = 0;        // Eack events enqueued to the RUE.
  uint32_t rue_eack_drop_events =
      0;                       // Eack events indicating drops to the RUE.
  uint32_t rue_responses = 0;  // Responses received back from the RUE.

  // RUE Event drop counters.
  // Flag: enable_per_connection_rue_drop_counters
  static constexpr std::string_view kRueEnqueueAttempts =
      "falcon.cid$0.counter.rue_enqueue_attempts";
  static constexpr std::string_view kRueEventDroppedAcks =
      "falcon.cid$0.counter.event_drops_ack";
  static constexpr std::string_view kRueEventDroppedNacks =
      "falcon.cid$0.counter.event_drops_nack";
  static constexpr std::string_view kRueEventDroppedRetransmits =
      "falcon.cid$0.counter.event_drops_retransmits";
  static constexpr std::string_view kRueEventDroppedEacks =
      "falcon.cid$0.counter.event_drops_eack";
  static constexpr std::string_view kRueEventDroppedEackDrops =
      "falcon.cid$0.counter.event_drops_eack_drop";
  uint32_t rue_enqueue_attempts = 0;  // Enqueue attempts to RUE.
  uint32_t rue_event_drops_ack = 0;   // Dropped ack packets at the event queue.
  uint32_t rue_event_drops_nack = 0;  // Dropped nack events at the event queue.
  uint32_t rue_event_drops_retransmit =
      0;  // Dropped retransmit events at the event queue.
  uint32_t rue_event_drops_eack = 0;  // Dropped eack events at the event queue.
  uint32_t rue_event_drops_eack_drop =
      0;  // Dropped eack-drop events at the event queue.

  // ACK generation counters.
  // Flag: enable_per_connection_ack_reason_counters
  static constexpr std::string_view kAcksGeneratedDueToTimeout =
      "falcon.cid$0.counter.acks_generated_due_to_timeout";
  static constexpr std::string_view kAcksGeneratedDueToAR =
      "falcon.cid$0.counter.acks_generated_due_to_ar";
  static constexpr std::string_view kAcksGeneratedDueToCoalescingCounter =
      "falcon.cid$0.counter.acks_generated_due_to_coalescing_counter";
  uint32_t acks_generated_due_to_ar = 0;
  uint32_t acks_generated_due_to_timeout = 0;
  uint32_t acks_generated_due_to_coalescing_counter = 0;

  // Packet drop counters.
  // Flag: enable_per_connection_packet_drop_counters
  static constexpr std::string_view kRxDroppedPackets =
      "falcon.cid$0.counter.total_rx_dropped_pkts";
  static constexpr std::string_view kRxDroppedRsrcPackets =
      "falcon.cid$0.counter.rx_resource_dropped_transaction_pkts";
  static constexpr std::string_view kRxDroppedWindowPackets =
      "falcon.cid$0.counter.rx_window_dropped_transaction_pkts";
  static constexpr std::string_view kRxDroppedDuplicatePackets =
      "falcon.cid$0.counter.rx_duplicate_dropped_transaction_pkts";
  static constexpr std::string_view kRxDroppedDuplicateAckPackets =
      "falcon.cid$0.counter.rx_duplicate_dropped_ack_pkts";
  static constexpr std::string_view kRxDroppedStaleAckPacketsWithZeroNumAcked =
      "falcon.cid$0.counter.rx_stale_dropped_ack_pkts_with_zero_num_acked";
  static constexpr std::string_view kRxDroppedDuplicateNackPackets =
      "falcon.cid$0.counter.rx_duplicate_dropped_nack_pkts";
  uint32_t total_rx_dropped_pkts = 0;
  uint32_t rx_resource_dropped_transaction_pkts =
      0;  // Transaction packets dropped due to resource unavailability.
  uint32_t rx_window_dropped_transaction_pkts =
      0;  // Transaction packets dropped due to being to the right of the
          // sliding window.
  uint32_t rx_duplicate_dropped_transaction_pkts =
      0;  // Transaction packets being dropped due to being to the left of the
          // sliding window or a spurious retransmission.
  uint32_t rx_duplicate_dropped_ack_pkts =
      0;  // ACK packets dropped due to being to the left of the sliding window.
  uint32_t rx_stale_dropped_ack_pkts_with_zero_num_acked =
      0;  // ACK packets dropped due to being stale with zero num acked.
  uint32_t rx_duplicate_dropped_nack_pkts =
      0;  // NACK packets dropped due to being to the left of the sliding
          // window.

  // Retransmission counters.
  // Flag: enable_per_connection_retx_counters
  static constexpr std::string_view kTxTimeoutRetransmitted =
      "falcon.cid$0.counter.tx_timeout_retransmitted";
  static constexpr std::string_view kTxEarlyOooDisRetransmitted =
      "falcon.cid$0.counter.tx_early_ooo_dis_retransmitted";
  static constexpr std::string_view kTxEarlyRackRetransmitted =
      "falcon.cid$0.counter.tx_early_rack_retransmitted";
  static constexpr std::string_view kTxEarlyNackRetransmitted =
      "falcon.cid$0.counter.tx_early_nack_retransmitted";
  static constexpr std::string_view kTxEarlyTlpRetransmitted =
      "falcon.cid$0.counter.tx_early_tlp_retransmitted";
  static constexpr std::string_view kTxUlpRetransmitted =
      "falcon.cid$0.counter.tx_ulp_retransmitted";
  uint32_t tx_timeout_retransmitted = 0;
  uint32_t tx_early_ooo_dis_retransmitted = 0;
  uint32_t tx_early_rack_retransmitted = 0;
  uint32_t tx_early_nack_retransmitted = 0;
  uint32_t tx_early_tlp_retransmitted = 0;
  uint32_t tx_ulp_retransmitted = 0;

  // Solicitation counters.
  // Flag: enable_per_connection_solicitation_counters
  static constexpr std::string_view kSolicitationWindowOccupiedBytes =
      "falcon.cid$0.counter.solicitation_window_occupied_bytes";
  uint32_t solicitation_window_occupied_bytes = 0;

  // Per-connection resource credit counters.
  // Flag: enable_per_connection_resource_credit_counters
  static constexpr std::string_view kPerConnTxPktCreditsUlpReqs =
      "falcon.cid$0.counter.tx_packet_credits.ulp_requests";
  static constexpr std::string_view kPerConnTxPktCreditsUlpData =
      "falcon.cid$0.counter.tx_packet_credits.ulp_data";
  static constexpr std::string_view kPerConnTxPktCreditsNtwkReqs =
      "falcon.cid$0.counter.tx_packet_credits.network_requests";
  static constexpr std::string_view kPerConnTxBufferCreditsUlpReqs =
      "falcon.cid$0.counter.tx_buffer_credits.ulp_requests";
  static constexpr std::string_view kPerConnTxBufferCreditsUlpData =
      "falcon.cid$0.counter.tx_buffer_credits.ulp_data";
  static constexpr std::string_view kPerConnTxBufferCreditsNtwkReqs =
      "falcon.cid$0.counter.tx_buffer_credits.network_requests";
  static constexpr std::string_view kPerConnRxPktCreditsUlpReqs =
      "falcon.cid$0.counter.rx_packet_credits.ulp_requests";
  static constexpr std::string_view kPerConnRxPktCreditsNtwkReqs =
      "falcon.cid$0.counter.rx_packet_credits.network_requests";
  static constexpr std::string_view kPerConnRxBufferCreditsUlpReqs =
      "falcon.cid$0.counter.rx_buffer_credits.ulp_requests";
  static constexpr std::string_view kPerConnRxBufferCreditsNtwkReqs =
      "falcon.cid$0.counter.rx_buffer_credits.network_requests";
  uint32_t tx_pkt_credits_ulp_requests = 0;
  uint32_t tx_pkt_credits_ulp_data = 0;
  uint32_t tx_pkt_credits_network_requests = 0;
  uint32_t tx_buf_credits_ulp_requests = 0;
  uint32_t tx_buf_credits_ulp_data = 0;
  uint32_t tx_buf_credits_network_requests = 0;
  uint32_t rx_pkt_credits_ulp_requests = 0;
  uint32_t rx_pkt_credits_network_requests = 0;
  uint32_t rx_buf_credits_ulp_requests = 0;
  uint32_t rx_buf_credits_network_requests = 0;

  // Congestion window pause time.
  // Flag: enable_per_connection_cwnd_pause
  static constexpr std::string_view kPerConnectionCwndPauseTime =
      "falcon.cid$0.counter.connection_cwnd_pause_time";
  double connection_cwnd_pause_time = 0;

  // Per-connection scheduler queue lengths.
  // Flag: enable_per_connection_scheduler_queue_length
  static constexpr std::string_view kPullAndOrderedPushRequestQueueCounter =
      "falcon.cid$0.counter.pull_and_ordered_push_request_queue";
  static constexpr std::string_view kUnorderedPushRequestQueueCounter =
      "falcon.cid$0.counter.unordered_push_request_queue";
  static constexpr std::string_view kPushDataQueueCounter =
      "falcon.cid$0.counter.push_data_queue";
  static constexpr std::string_view kPushGrantQueueCounter =
      "falcon.cid$0.counter.push_grant_queue";
  static constexpr std::string_view kPullDataQueueCounter =
      "falcon.cid$0.counter.pull_data_queue";
  uint32_t pull_and_ordered_push_request_queue_packets = 0;
  uint32_t unordered_push_request_queue_packets = 0;
  uint32_t push_data_queue_packets = 0;
  uint32_t push_grant_queue_packets = 0;
  uint32_t pull_data_queue_packets = 0;

  // Max RSN difference between Pull Request and Push Unsolicited Data.
  // Flag: enable_per_connection_max_rsn_difference
  static constexpr std::string_view kMaxPullPushRsnDistance =
      "falcon.cid$0.counter.max_pull_push_rsn_distance";
  uint32_t max_pull_push_unsolicited_rsn_distance = 0;

  std::string DebugString() const;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_COUNTERS_H_
