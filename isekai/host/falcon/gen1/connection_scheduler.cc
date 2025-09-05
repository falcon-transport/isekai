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

#include "isekai/host/falcon/gen1/connection_scheduler.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/substitute.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_histograms.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/falcon/gen1/weighted_round_robin_policy.h"

namespace isekai {
namespace {
constexpr double kPacketTypeWeightNormalizer = 64.0;
const std::vector<PacketTypeQueue> kQueueTypes(
    {PacketTypeQueue::kPullAndOrderedPushRequest,
     PacketTypeQueue::kUnorderedPushRequest, PacketTypeQueue::kPushData,
     PacketTypeQueue::kPushGrant, PacketTypeQueue::kPullData});
//
// should be RDMA_MTU + RDMA_HEADERS. In practice, we can substitute network MTU
// and use it instead. 4200 is a conservative estimate of 4096 + headers.
constexpr uint64_t kMaxRequestLength = 4200;
}  // namespace

ProtocolConnectionScheduler::ProtocolConnectionScheduler(
    FalconModelInterface* falcon)
    : packet_type_policy_(
          /*fetcher=*/
          [this](const PacketTypeQueue& packet_type) -> int {
            return ComputePacketTypeWeight(packet_type);
          },
          /*batched=*/false, /*enforce_order*/ true),
      phantom_connection_policy_(nullptr),
      ordered_push_req_connection_policy_(nullptr),
      falcon_(falcon) {
  for (const auto& queue : kQueueTypes) {
    // Initialize per-packet_type counters to zero.
    packet_queue_wise_pkt_count_[queue] = 0;
    // Add packet_type to the round robin policy.
    CHECK_OK(packet_type_policy_.InitializeEntity(queue));  // Crash OK.
    // Initialize connection policy for the packet type.
    connection_policy_.emplace(
        queue,
        WeightedRoundRobinPolicy<uint32_t>(
            /*fetcher=*/nullptr, /*batched=*/false, /*enforce_order*/ true));
  }

  // Initialize the per-packet type tx counters
  std::vector<falcon::PacketType> packet_types({
      falcon::PacketType::kInvalid,
      falcon::PacketType::kPullRequest,
      falcon::PacketType::kPushRequest,
      falcon::PacketType::kPushGrant,
      falcon::PacketType::kPullData,
      falcon::PacketType::kPushSolicitedData,
      falcon::PacketType::kPushUnsolicitedData,
  });
  for (const auto& packet_type : packet_types) {
    packet_type_wise_tx_count_[packet_type] = 0;
    packet_type_wise_max_queueing_delay_[packet_type] = absl::ZeroDuration();
  }
  collect_max_delay_stats_ =
      falcon_->get_stats_manager()
          ->GetStatsConfig()
          .enable_connection_scheduler_max_delayed_packet_stats();
}

absl::Status ProtocolConnectionScheduler::InitConnectionSchedulerQueues(
    uint32_t scid) {
  VLOG(2) << "[Scheduler] Initialize connection queues for cid: " << scid;
  auto [unused_iterator, was_inserted] =
      connection_queues_.insert({scid, ConnectionSchedulerQueues()});
  if (!was_inserted) {
    return absl::AlreadyExistsError(
        "Duplicate source connection ID. Queues already exist.");
  }
  // Add connection to each per-packet_type connection policy.
  for (const auto& queue : kQueueTypes) {
    RETURN_IF_ERROR(connection_policy_.at(queue).InitializeEntity(scid));
  }
  CHECK_OK(ordered_push_req_connection_policy_.InitializeEntity(scid));
  CHECK_OK(phantom_connection_policy_.InitializeEntity(scid));
  return absl::OkStatus();
}

absl::Status ProtocolConnectionScheduler::EnqueuePacket(
    uint32_t scid, uint32_t rsn, falcon::PacketType type) {
  VLOG(2) << "[Scheduler] Enqueue packet. "
          << absl::StrFormat("cid: %lu, rsn: %lu, pkt_type: %d", scid, rsn,
                             static_cast<int>(type));
  //
  // this function.
  AddPacketToQueue(scid, rsn, type);

  VLOG(2) << "Recompute after enqueue";
  RecomputeEligibility(scid);

  // Schedule the work scheduler in case this is the only outstanding work item.
  falcon_->get_arbiter()->ScheduleSchedulerArbiter();
  return absl::OkStatus();
}

PacketTypeQueue ProtocolConnectionScheduler::AddPacketToQueue(
    uint32_t scid, uint32_t rsn, falcon::PacketType type) {
  VLOG(2) << "[" << falcon_->get_host_id() << ": "
          << falcon_->get_environment()->ElapsedTime() << "][" << scid << ", "
          << rsn << ", " << static_cast<int>(type) << "] "
          << "Enqueues in the connection scheduler.";
  falcon_->get_stats_manager()->UpdateSchedulerCounters(
      SchedulerTypes::kConnection, false);

  // Decide the connection queue to which this packet needs to be added to based
  // on its type and whether the connection is ordered or not.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(ConnectionState* const connection_state,
                       state_manager->PerformDirectLookup(scid));
  // Fetching transaction and packet metadata to record enqueue timestamps.
  CHECK_OK_THEN_ASSIGN(
      auto transaction,
      connection_state->GetTransaction(TransactionKey(
          rsn, GetTransactionLocation(type, /*incoming=*/false))));
  CHECK_OK_THEN_ASSIGN(auto packet_metadata,
                       transaction->GetPacketMetadata(type));
  bool is_ordered_connection =
      connection_state->connection_metadata->ordered_mode ==
              OrderingMode::kOrdered
          ? true
          : false;

  PacketTypeQueue queue_type;
  switch (type) {
    case falcon::PacketType::kPullRequest:
      queue_type = PacketTypeQueue::kPullAndOrderedPushRequest;
      break;
    case falcon::PacketType::kPushRequest:
      if (is_ordered_connection) {
        queue_type = PacketTypeQueue::kPullAndOrderedPushRequest;
      } else {
        queue_type = PacketTypeQueue::kUnorderedPushRequest;
      }
      break;
    case falcon::PacketType::kPullData:
      queue_type = PacketTypeQueue::kPullData;
      break;
    case falcon::PacketType::kPushGrant:
      queue_type = PacketTypeQueue::kPushGrant;
      break;
    case falcon::PacketType::kPushSolicitedData: {
      queue_type = PacketTypeQueue::kPushData;
      if (is_ordered_connection) {
        packet_metadata->queuing_delay_metadata.tx_eligible_time =
            falcon_->get_environment()->ElapsedTime();
        packet_metadata->is_transmission_eligible = true;
        // Take snapshot of per-packet type TX counters
        packet_metadata->queuing_delay_metadata
            .enqueue_packet_type_wise_tx_count_snapshot =
            packet_type_wise_tx_count_;
        // Take snapshot of number of packets in each packet-type queue
        packet_metadata->queuing_delay_metadata
            .enqueue_packet_queue_wise_pkt_count_snapshot =
            packet_queue_wise_pkt_count_;
        // Update the count corresponding to this queue as the push solicited
        // data is eligible for transmission.
        UpdateOutstandingPacketCount(queue_type, /*is_dequed=*/false);
      }
    } break;
    case falcon::PacketType::kPushUnsolicitedData:
      queue_type = PacketTypeQueue::kPushData;
      break;
    case falcon::PacketType::kNack:
      LOG(FATAL) << "NACKs should not go through connection scheduler.";
      break;
    case falcon::PacketType::kResync:
      LOG(FATAL) << "Logic for handling resync packet not implemented.";
      break;
    default:
      LOG(FATAL) << "Not a FALCON packet type enqueued.";
      break;
  }

  // Update stats, enqueue in relevant queue for all types leaving ordered push
  // solicited data. For this type, the packet is enqueued along with the push
  // request.
  if (!is_ordered_connection ||
      type != falcon::PacketType::kPushSolicitedData) {
    // Update intra connection scheduler queue statistifcs.
    falcon_->get_stats_manager()->UpdateIntraConnectionSchedulerCounters(
        scid, queue_type, false);
    packet_metadata->queuing_delay_metadata.enqueue_time =
        falcon_->get_environment()->ElapsedTime();
    packet_metadata->queuing_delay_metadata
        .enqueue_packet_type_wise_tx_count_snapshot =
        packet_type_wise_tx_count_;
    packet_metadata->queuing_delay_metadata
        .enqueue_packet_queue_wise_pkt_count_snapshot =
        packet_queue_wise_pkt_count_;

    // Create work id that is going to enqueued in the appropriate queue.
    WorkId work_id(rsn, type);
    connection_queues_[scid].Enqueue(queue_type, work_id);
    UpdateOutstandingPacketCount(queue_type, /*is_dequed=*/false);
  }

  // Enqueues the corresponding phantom request for the push unsolicited data,
  // and lets the request queue active.
  if (is_ordered_connection &&
      type == falcon::PacketType::kPushUnsolicitedData) {
    WorkId phantom_request_work_id(rsn, falcon::PacketType::kInvalid);
    queue_type = PacketTypeQueue::kPullAndOrderedPushRequest;
    // Update the counter corresponding to this queue as a phantom request is
    // added which is eligible for service.
    UpdateOutstandingPacketCount(queue_type, /*is_dequed=*/false);
    CHECK_OK_THEN_ASSIGN(
        auto phantom_request_packet_metadata,
        transaction->GetPacketMetadata(falcon::PacketType::kInvalid));
    phantom_request_packet_metadata->queuing_delay_metadata.enqueue_time =
        falcon_->get_environment()->ElapsedTime();
    phantom_request_packet_metadata->queuing_delay_metadata
        .enqueue_packet_type_wise_tx_count_snapshot =
        packet_type_wise_tx_count_;
    phantom_request_packet_metadata->queuing_delay_metadata
        .enqueue_packet_queue_wise_pkt_count_snapshot =
        packet_queue_wise_pkt_count_;
    connection_queues_[scid].Enqueue(queue_type, phantom_request_work_id);
  }

  // In case of ordered push solicited request, we must also enqueue the push
  // solicited data in the relevant queue at request insertion time itself to
  // ensure ordering between solicited data and unsolicited data.
  if (is_ordered_connection && type == falcon::PacketType::kPushRequest) {
    WorkId data_work_id(rsn, falcon::PacketType::kPushSolicitedData);
    // This queue is not marked active currently as this solicited data
    // becomes eligible only when we receive the grant.
    auto push_data_queue = PacketTypeQueue::kPushData;
    falcon_->get_stats_manager()->UpdateIntraConnectionSchedulerCounters(
        scid, push_data_queue, false);
    CHECK_OK_THEN_ASSIGN(
        auto push_data_packet_metadata,
        transaction->GetPacketMetadata(falcon::PacketType::kPushSolicitedData));
    push_data_packet_metadata->queuing_delay_metadata.enqueue_time =
        falcon_->get_environment()->ElapsedTime();
    push_data_packet_metadata->queuing_delay_metadata
        .enqueue_packet_type_wise_tx_count_snapshot =
        packet_type_wise_tx_count_;
    push_data_packet_metadata->queuing_delay_metadata
        .enqueue_packet_queue_wise_pkt_count_snapshot =
        packet_queue_wise_pkt_count_;

    connection_queues_[scid].Enqueue(push_data_queue, data_work_id);
  }

  return queue_type;
}

bool ProtocolConnectionScheduler::HasWork() {
  return phantom_connection_policy_.HasWork() ||
         (falcon_->CanSendPacket() && packet_type_policy_.HasWork());
}

bool ProtocolConnectionScheduler::ScheduleWork() {
  VLOG(2) << "[Scheduler] Scheduling next work. ";

  // Pick a packet type and connection_id to service in this cycle.
  absl::StatusOr<PacketTypeQueue> candidate_packet_type_queue;
  absl::StatusOr<uint32_t> candidate_connection_id;

  if (falcon_->CanSendPacket()) {
    candidate_packet_type_queue = packet_type_policy_.GetNextEntity();
    CHECK(candidate_packet_type_queue.ok());  // Crash OK.
    candidate_connection_id =
        connection_policy_.at(candidate_packet_type_queue.value())
            .GetNextEntity();
  } else {
    // We are xoff'ed by PB, so we must schedule a phantom packet/queue.
    candidate_packet_type_queue = PacketTypeQueue::kPullAndOrderedPushRequest;
    // Select a connection that has an eligible phantom packet.
    candidate_connection_id = phantom_connection_policy_.GetNextEntity();
  }

  CHECK(candidate_connection_id.ok());  // Crash OK.
  int scid = candidate_connection_id.value();

  VLOG(2) << "Next packet_type, cid: "
          << static_cast<int>(candidate_packet_type_queue.value()) << ", "
          << scid;

  const WorkId& work_id =
      connection_queues_[scid].Peek(candidate_packet_type_queue.value());

  if (work_id.type == falcon::PacketType::kInvalid) {
    HandlePhantomRequest(candidate_connection_id.value(), work_id,
                         candidate_packet_type_queue.value());
    return true;
  }

  // This check should not fail if gating variables are maintained correctly.
  CHECK(CanSend(scid, work_id.rsn, work_id.type));  // Crash OK.

  SchedulePacket(scid, candidate_packet_type_queue.value(), work_id);
  return true;
}

void ProtocolConnectionScheduler::UpdateQueuingDelayStatistics(
    PacketMetadata::PacketQueuingDelayMetadata* queuing_delay_metadata,
    uint32_t scid, bool is_ordered_connection, falcon::PacketType packet_type) {
  absl::Duration queueing_delay = falcon_->get_environment()->ElapsedTime() -
                                  queuing_delay_metadata->enqueue_time;
  absl::Duration tx_eligible_queueing_delay =
      falcon_->get_environment()->ElapsedTime() -
      queuing_delay_metadata->tx_eligible_time;
  StatisticCollectionInterface* stats_collector =
      falcon_->get_stats_collector();
  if (collect_max_delay_stats_ && stats_collector) {
    // We choose for data packets the delay since they became TX-eligible.
    absl::Duration chosen_queueing_delay =
        packet_type == falcon::PacketType::kPushSolicitedData ||
                packet_type == falcon::PacketType::kPushUnsolicitedData
            ? tx_eligible_queueing_delay
            : queueing_delay;
    // If the queuing delay of this packet is the max seen for this type:
    if (chosen_queueing_delay >
        packet_type_wise_max_queueing_delay_[packet_type]) {
      // Update the max queueing delay counter for this type;
      packet_type_wise_max_queueing_delay_[packet_type] = chosen_queueing_delay;
      CHECK_OK(falcon_->get_stats_collector()->UpdateStatistic(
          absl::Substitute(kMaxQueueingDelayByType, TypeToString(packet_type)),
          absl::ToDoubleMicroseconds(
              packet_type_wise_max_queueing_delay_[packet_type]),
          StatisticsCollectionConfig::TIME_SERIES_STAT));
      // For each packet type:
      for (const auto& type_count_snapshot :
           queuing_delay_metadata->enqueue_packet_type_wise_tx_count_snapshot) {
        // Record difference between current per-packet type TX counter and the
        // snapshot when this packet was enqueued;
        CHECK_OK(falcon_->get_stats_collector()->UpdateStatistic(
            absl::Substitute(kSerializedPacketCountByTypeForMaxQueuedPacket,
                             TypeToString(packet_type),
                             TypeToString(type_count_snapshot.first)),
            packet_type_wise_tx_count_[type_count_snapshot.first] -
                type_count_snapshot.second,
            StatisticsCollectionConfig::TIME_SERIES_STAT));
      }
      // Also, record the queue-wise counter snapshot taken when this packet was
      // enqueued.
      for (const auto& type_count_snapshot :
           queuing_delay_metadata
               ->enqueue_packet_queue_wise_pkt_count_snapshot) {
        CHECK_OK(falcon_->get_stats_collector()->UpdateStatistic(
            absl::Substitute(kEnqueueTimeQueueLengthForMaxQueuedPacket,
                             TypeToString(packet_type),
                             TypeToString(type_count_snapshot.first)),
            type_count_snapshot.second,
            StatisticsCollectionConfig::TIME_SERIES_STAT));
      }
    }
  }
  // Record the difference between now and the enqueue of this packet as a data
  // point of queuing delay for this type of packets.
  falcon_->get_histogram_collector()->Add(
      scid, packet_type, is_ordered_connection, false,
      absl::ToDoubleNanoseconds(queueing_delay));
  if (packet_type == falcon::PacketType::kPushSolicitedData) {
    // Solicited data is enqueued when the request is enqueued but is not yet
    // TX-eligible. We also record The time since it became TX-eligible on
    // grant.
    falcon_->get_histogram_collector()->Add(
        scid, packet_type, is_ordered_connection, true,
        absl::ToDoubleNanoseconds(tx_eligible_queueing_delay));
  }
}

// Return the total queue length of an inter-connection queue.
uint32_t ProtocolConnectionScheduler::GetConnectionQueueLength(uint32_t scid) {
  uint32_t unscheduled_queue_length = 0;
  unscheduled_queue_length += connection_queues_[scid].GetSize(
      PacketTypeQueue::kPullAndOrderedPushRequest);
  unscheduled_queue_length +=
      connection_queues_[scid].GetSize(PacketTypeQueue::kUnorderedPushRequest);
  unscheduled_queue_length +=
      connection_queues_[scid].GetSize(PacketTypeQueue::kPushData);
  unscheduled_queue_length +=
      connection_queues_[scid].GetSize(PacketTypeQueue::kPushGrant);
  unscheduled_queue_length +=
      connection_queues_[scid].GetSize(PacketTypeQueue::kPullData);
  return unscheduled_queue_length;
}

void ProtocolConnectionScheduler::RecomputeEligibility(uint32_t scid) {
  VLOG(2) << "[Scheduler] Recompute Eligibility, cid: " << scid << ", time: "
          << absl::ToInt64Nanoseconds(
                 falcon_->get_environment()->ElapsedTime());

  // We recompute the eligibility of the scheduler in two steps.

  // 1. If the scid is valid, we recompute the eligibilty of each packet type
  //     queue within the connection. We also update phantom and ordered_push
  //     policy in this step.

  // 2. Next, we update the policy across all packet type queues based on global
  //    criteria or if any connection for the given packet type became eligible.

  if (connection_queues_.contains(scid)) {
    ConnectionStateManager* const conn_state_manager =
        falcon_->get_state_manager();
    CHECK_OK_THEN_ASSIGN(auto connection_state,
                         conn_state_manager->PerformDirectLookup(scid));

    bool meets_op_rate_criteria = (connection_state->is_op_rate_limited == 0);

    for (const auto& queue_type : kQueueTypes) {
      // If the queue is empty, mark it as inactive.
      if (connection_queues_[scid].IsEmpty(queue_type)) {
        VLOG(2) << "Marking queue_type inactive due to empty: "
                << static_cast<int>(queue_type);
        connection_policy_.at(queue_type).MarkEntityInactive(scid);
        // If PullAndOrderedPushRequest queue, mark phantom and ordered_push_req
        // policy as inactive too.
        if (queue_type == PacketTypeQueue::kPullAndOrderedPushRequest) {
          phantom_connection_policy_.MarkEntityInactive(scid);
          ordered_push_req_connection_policy_.MarkEntityInactive(scid);
        }
        continue;
      }

      // Peek into the HoL queue entry.
      const WorkId& work_id = connection_queues_[scid].Peek(queue_type);

      // If phantom request at HoL, mark queue type as active.
      if (queue_type == PacketTypeQueue::kPullAndOrderedPushRequest) {
        if (work_id.type == falcon::PacketType::kInvalid) {
          connection_policy_.at(queue_type).MarkEntityActive(scid);
          phantom_connection_policy_.MarkEntityActive(scid);
          VLOG(2) << "Marking queue_type active due to phantom: "
                  << static_cast<int>(queue_type);
          continue;
        } else {
          phantom_connection_policy_.MarkEntityInactive(scid);
        }
      }

      // If the connection does not meet op-rate criteria, mark queue inactive.
      if (!meets_op_rate_criteria) {
        VLOG(2) << "Marking queue type inactive due to op_rate " << scid << " "
                << static_cast<int>(queue_type) << " "
                << connection_state->last_packet_send_time;
        connection_policy_.at(queue_type).MarkEntityInactive(scid);
        continue;
      }

      bool is_packet_eligible = true;
      if (queue_type == PacketTypeQueue::kPushData) {
        CHECK_OK_THEN_ASSIGN(
            auto transaction,
            connection_state->GetTransaction(TransactionKey(
                work_id.rsn, GetTransactionLocation(work_id.type,
                                                    /*incoming=*/false))));
        CHECK_OK_THEN_ASSIGN(auto packet_metadata,
                             transaction->GetPacketMetadata(work_id.type));
        is_packet_eligible = packet_metadata->is_transmission_eligible;
      }

      // Check for CC and OOW criteria.
      bool meets_cc_criteria =
          falcon_->get_packet_reliability_manager()
              ->MeetsInitialTransmissionCCTxGatingCriteria(scid, work_id.type);
      bool meets_oow_criteria =
          falcon_->get_packet_reliability_manager()
              ->MeetsInitialTransmissionOowTxGatingCriteria(scid, work_id.type);

      if (is_packet_eligible && meets_cc_criteria && meets_oow_criteria) {
        VLOG(2) << "Marking queue_type active due to all criteria pass: "
                << static_cast<int>(queue_type);
        connection_policy_.at(queue_type).MarkEntityActive(scid);
      } else {
        VLOG(2) << "Marking queue_type inactive due to failing criteria: "
                << static_cast<int>(queue_type) << " " << is_packet_eligible
                << " " << meets_cc_criteria << " " << meets_oow_criteria;
        connection_policy_.at(queue_type).MarkEntityInactive(scid);
      }

      // If HoL entry is a PushRequest for PullAndOrderedPushRequest queue, then
      // mark ordered_push_req policy as active for the connection.
      if (queue_type == PacketTypeQueue::kPullAndOrderedPushRequest) {
        if (work_id.type == falcon::PacketType::kPushRequest &&
            meets_cc_criteria && meets_oow_criteria) {
          ordered_push_req_connection_policy_.MarkEntityActive(scid);
        } else {
          ordered_push_req_connection_policy_.MarkEntityInactive(scid);
        }
      }
    }
  }

  // Evaluate global packet_type criteria and update packet_type policy.
  for (const auto& queue_type : kQueueTypes) {
    bool meets_admission_control_criteria = true;
    if (queue_type == PacketTypeQueue::kPushGrant) {
      meets_admission_control_criteria =
          falcon_->get_admission_control_manager()
              ->MeetsAdmissionControlCriteria(kMaxRequestLength,
                                              falcon::PacketType::kPushGrant);
    } else if (queue_type == PacketTypeQueue::kPullAndOrderedPushRequest) {
      meets_admission_control_criteria =
          ordered_push_req_connection_policy_.HasWork() ||
          phantom_connection_policy_.HasWork() ||
          falcon_->get_admission_control_manager()
              ->MeetsAdmissionControlCriteria(kMaxRequestLength,
                                              falcon::PacketType::kPullRequest);
    }

    if (meets_admission_control_criteria &&
        connection_policy_.at(queue_type).HasWork()) {
      VLOG(3) << "Global " << static_cast<int>(queue_type) << " active.";
      packet_type_policy_.MarkEntityActive(queue_type);
    } else {
      VLOG(3) << "Global " << static_cast<int>(queue_type) << " inactive.";
      packet_type_policy_.MarkEntityInactive(queue_type);
    }
  }

  // Schedule arbiter if there is work for the connection scheduler.
  if (HasWork()) {
    falcon_->get_arbiter()->ScheduleSchedulerArbiter();
  }
}

bool ProtocolConnectionScheduler::CanSend(uint32_t scid, uint32_t rsn,
                                          falcon::PacketType type) {
  ConnectionStateManager* const conn_state_manager =
      falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       conn_state_manager->PerformDirectLookup(scid));
  CHECK_OK_THEN_ASSIGN(auto transaction,
                       connection_state->GetTransaction(TransactionKey(
                           rsn, GetTransactionLocation(type,
                                                       /*incoming=*/false))));
  CHECK_OK_THEN_ASSIGN(auto packet_metadata,
                       transaction->GetPacketMetadata(type));

  bool meets_cc_criteria =
      falcon_->get_packet_reliability_manager()
          ->MeetsInitialTransmissionCCTxGatingCriteria(scid, type);
  bool meets_admission_control_criteria =
      falcon_->get_admission_control_manager()->MeetsAdmissionControlCriteria(
          scid, rsn, type);
  bool meets_oow_criteria =
      falcon_->get_packet_reliability_manager()
          ->MeetsInitialTransmissionOowTxGatingCriteria(scid, type);
  bool meets_op_rate_criteria = (connection_state->is_op_rate_limited == 0);

  falcon_->get_stats_manager()->UpdateCwndPauseCounters(scid,
                                                        !meets_cc_criteria);

  return packet_metadata->is_transmission_eligible && meets_op_rate_criteria &&
         meets_cc_criteria && meets_admission_control_criteria &&
         meets_oow_criteria;
}

// Schedules the given packet by handing it off to the downstream blocks.
void ProtocolConnectionScheduler::SchedulePacket(uint32_t scid,
                                                 PacketTypeQueue queue_type,
                                                 const WorkId& work_id) {
  VLOG(2) << "[" << falcon_->get_host_id() << ": "
          << falcon_->get_environment()->ElapsedTime() << "][" << scid << ", "
          << work_id.rsn << ", " << static_cast<int>(work_id.type) << "] "
          << "Packet in connection scheduler picked by arbiter.";
  auto stats_manager = falcon_->get_stats_manager();
  stats_manager->UpdateSchedulerCounters(SchedulerTypes::kConnection, true);

  CHECK_OK(
      falcon_->get_admission_control_manager()->ReserveAdmissionControlResource(
          scid, work_id.rsn, work_id.type));

  // Update intra connection scheduler queue statisitcs.
  stats_manager->UpdateIntraConnectionSchedulerCounters(scid, queue_type, true);
  UpdateOutstandingPacketCount(queue_type, true);

  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       state_manager->PerformDirectLookup(scid));
  bool is_ordered_connection =
      connection_state->connection_metadata->ordered_mode ==
              OrderingMode::kOrdered
          ? true
          : false;
  const bool is_incoming_packet = false;
  CHECK_OK_THEN_ASSIGN(
      auto transaction,
      connection_state->GetTransaction(TransactionKey(
          work_id.rsn,
          GetTransactionLocation(work_id.type, is_incoming_packet))));
  CHECK_OK_THEN_ASSIGN(auto packet_metadata,
                       transaction->GetPacketMetadata(work_id.type));

  // Update last packet send time for this connection to enforce op-rate limits.
  connection_state->last_packet_send_time =
      falcon_->get_environment()->ElapsedTime();
  connection_state->is_op_rate_limited = 1;

  // Schedule an event in the future to mark the connection as op_rate eligible.
  CHECK_OK(falcon_->get_environment()->ScheduleEvent(
      kPerConnectionInterOpGap, [this, scid]() {
        ConnectionStateManager* const conn_state_manager =
            falcon_->get_state_manager();
        CHECK_OK_THEN_ASSIGN(auto connection_state,
                             conn_state_manager->PerformDirectLookup(scid));
        connection_state->is_op_rate_limited = 0;
      }));

  packet_metadata->schedule_status = absl::OkStatus();

  UpdateQueuingDelayStatistics(&packet_metadata->queuing_delay_metadata, scid,
                               is_ordered_connection, work_id.type);
  RecordRsnDiffMeasure(scid, work_id);

  falcon_->get_stats_manager()->UpdateInitialTxRsnSeries(scid, work_id.rsn,
                                                         work_id.type);

  CHECK_OK(falcon_->get_packet_reliability_manager()->TransmitPacket(
      scid, work_id.rsn, work_id.type));

  connection_queues_[scid].Pop(queue_type);

  //
  VLOG(2) << "Recompute after schedule";
  RecomputeEligibility(scid);
}

void ProtocolConnectionScheduler::HandlePhantomRequest(
    uint32_t scid, const WorkId& work_id,
    PacketTypeQueue candidate_queue_type) {
  VLOG(2) << "[" << falcon_->get_host_id() << ": "
          << falcon_->get_environment()->ElapsedTime() << "][" << scid << ", "
          << work_id.rsn << ", " << static_cast<int>(work_id.type) << "] "
          << "Phantom request being processed.";
  // Marks the corresponding push unsolicited data as eligible to send.
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       falcon_->get_state_manager()->PerformDirectLookup(scid));
  CHECK_OK_THEN_ASSIGN(
      auto transaction,
      connection_state->GetTransaction(TransactionKey(
          work_id.rsn, GetTransactionLocation(
                           falcon::PacketType::kPushUnsolicitedData, false))));
  CHECK_OK_THEN_ASSIGN(
      auto push_unsolicited_packet,
      transaction->GetPacketMetadata(falcon::PacketType::kPushUnsolicitedData));
  push_unsolicited_packet->is_transmission_eligible = true;
  push_unsolicited_packet->queuing_delay_metadata.tx_eligible_time =
      falcon_->get_environment()->ElapsedTime();

  // Pops the phantom request and cleans up the request queue.
  CHECK_OK_THEN_ASSIGN(auto phantom_packet,
                       transaction->GetPacketMetadata(work_id.type));
  phantom_packet->schedule_status = absl::OkStatus();
  falcon_->get_histogram_collector()->Add(
      scid, work_id.type, true, false,
      absl::ToDoubleNanoseconds(
          falcon_->get_environment()->ElapsedTime() -
          phantom_packet->queuing_delay_metadata.enqueue_time));
  connection_queues_[scid].Pop(candidate_queue_type);
  // Reflect the handling of the phantom request by updating the outstanding
  // packet count corresponding to the relevant queue.
  UpdateOutstandingPacketCount(candidate_queue_type, /*is_dequed = */ true);

  //
  VLOG(2) << "Recompute after phantom";
  RecomputeEligibility(scid);
}

int ProtocolConnectionScheduler::ComputePacketTypeWeight(
    const PacketTypeQueue& packet_type) {
  int weight = std::ceil(packet_queue_wise_pkt_count_[packet_type] /
                         kPacketTypeWeightNormalizer);
  VLOG(3) << "Setting weight of queue_type " << static_cast<int>(packet_type)
          << " to " << weight;
  return weight;
}

void ProtocolConnectionScheduler::UpdateOutstandingPacketCount(
    PacketTypeQueue queue_type, bool is_dequed) {
  if (is_dequed) {
    packet_queue_wise_pkt_count_[queue_type] -= 1;
  } else {
    packet_queue_wise_pkt_count_[queue_type] += 1;
  }
  VLOG(3) << "Outstanding packet count " << static_cast<int>(queue_type) << " "
          << packet_queue_wise_pkt_count_[queue_type];
}

void ProtocolConnectionScheduler::RecordRsnDiffMeasure(uint32_t scid,
                                                       const WorkId& work_id) {
  if (work_id.type == falcon::PacketType::kPullRequest) {
    if (sent_out_push_unsolicited_data &&
        (work_id.rsn > last_push_unsolicited_data_rsn)) {
      auto rsn_diff_measure = work_id.rsn - last_push_unsolicited_data_rsn;
      falcon_->get_stats_manager()->UpdateMaxRsnDistance(scid,
                                                         rsn_diff_measure);
    }
  } else if (work_id.type == falcon::PacketType::kPushUnsolicitedData) {
    sent_out_push_unsolicited_data = true;
    last_push_unsolicited_data_rsn = work_id.rsn;
  }
}

}  // namespace isekai
