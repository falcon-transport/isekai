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

#include "isekai/host/falcon/gen1/retransmission_scheduler.h"

#include <cstdint>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
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

ProtocolRetransmissionScheduler::ProtocolRetransmissionScheduler(
    FalconModelInterface* falcon)
    : connection_policy_(/*fetcher=*/nullptr, /*batched=*/false,
                         /*enforce_order=*/true),
      falcon_(falcon) {
  if (falcon_->get_config()
          ->inter_connection_retransmission_scheduling_policy() !=
      FalconConfig::ROUND_ROBIN) {
    LOG(FATAL) << "Unsupported scheduling policy provided.";
  }
  if (falcon_->get_config()
          ->intra_connection_retransmission_scheduling_policy() !=
      FalconConfig::ROUND_ROBIN) {
    LOG(FATAL) << "Unsupported scheduling policy provided.";
  }
}

absl::Status ProtocolRetransmissionScheduler::InitConnectionSchedulerQueues(
    uint32_t scid) {
  VLOG(2) << "[Scheduler] Initialize retx connection queues for cid: " << scid;
  auto [connection_queues_unused_iterator, connection_queues_was_inserted] =
      connection_queues_.insert({scid, RetransmissionSchedulerQueues()});
  if (!connection_queues_was_inserted) {
    return absl::AlreadyExistsError(
        "Duplicate source connection ID. Queues already exist.");
  }
  // Add connection to policy and initialize next window type.
  CHECK_OK(connection_policy_.InitializeEntity(scid));
  next_window_type_[scid] = WindowTypeQueue::kRequest;

  return absl::OkStatus();
}

// Add a packet to the relevant queue for transmitting over the network.
absl::Status ProtocolRetransmissionScheduler::EnqueuePacket(
    uint32_t scid, uint32_t rsn, falcon::PacketType type) {
  VLOG(2) << "[" << falcon_->get_host_id() << ": "
          << falcon_->get_environment()->ElapsedTime() << "][" << scid << ", "
          << rsn << ", " << static_cast<int>(type) << "] "
          << "Enqueues in the retransmission scheduler.";
  falcon_->get_stats_manager()->UpdateSchedulerCounters(
      SchedulerTypes::kRetransmission, false);

  // Decide the connection queue to which this packet needs to be added to based
  // on its type and whether the connection is ordered or not.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(scid));
  ASSIGN_OR_RETURN(TransactionMetadata* const transaction,
                   connection_state->GetTransaction(
                       TransactionKey(rsn, GetTransactionLocation(
                                               /*type=*/type,
                                               /*incoming=*/false))));
  ASSIGN_OR_RETURN(PacketMetadata* const packet_metadata,
                   transaction->GetPacketMetadata(type));

  WindowTypeQueue queue_type;
  switch (type) {
    case falcon::PacketType::kPullRequest:
    case falcon::PacketType::kPushRequest:
      queue_type = WindowTypeQueue::kRequest;
      break;
    case falcon::PacketType::kPullData:
    case falcon::PacketType::kPushGrant:
    case falcon::PacketType::kPushSolicitedData:
    case falcon::PacketType::kPushUnsolicitedData:
      queue_type = WindowTypeQueue::kData;
      break;
    case falcon::PacketType::kAck:
      LOG(FATAL) << "ACKs should not go through retransmission scheduler.";
      break;
    case falcon::PacketType::kNack:
      LOG(FATAL) << "NACKs should not go through retransmission scheduler.";
      break;
    case falcon::PacketType::kResync:
      LOG(FATAL) << "Logic for handling resync packet not implemented.";
      break;
    case falcon::PacketType::kBack:
      LOG(FATAL) << "BACKs should not go through retransmission scheduler.";
      break;
    case falcon::PacketType::kEack:
      LOG(FATAL) << "EACKs should not go through retransmission scheduler.";
      break;
    case falcon::PacketType::kInvalid:
      LOG(FATAL)
          << "Invalid (phantoms) should not through retransmission scheduler.";
      break;
  }
  // Create work id that is going to enqueued in the appropriate queue.
  RetransmissionWorkId work_id(rsn, packet_metadata->psn, type);
  auto inserted = connection_queues_[scid].Enqueue(queue_type, work_id);
  if (!inserted) {
    return absl::AlreadyExistsError(
        "Previous retransmission of packet still in retransmission scheduler "
        "queue.");
  }

  VLOG(2) << "Recompute after enqueue";
  RecomputeEligibility(scid);

  // Schedule the work scheduler in case there is new work.
  falcon_->get_arbiter()->ScheduleSchedulerArbiter();
  return absl::OkStatus();
}

absl::Status ProtocolRetransmissionScheduler::DequeuePacket(
    uint32_t scid, uint32_t rsn, falcon::PacketType type) {
  VLOG(2) << "[" << falcon_->get_host_id() << ": "
          << falcon_->get_environment()->ElapsedTime() << "][" << scid << ", "
          << rsn << ", " << static_cast<int>(type) << "] "
          << "Dequeues in the retransmission scheduler.";
  // Decide the connection queue from which this packet needs to be added to
  // based on its type and whether the connection is ordered or not.
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  ASSIGN_OR_RETURN(ConnectionState* const connection_state,
                   state_manager->PerformDirectLookup(scid));
  ASSIGN_OR_RETURN(TransactionMetadata* const transaction,
                   connection_state->GetTransaction(
                       TransactionKey(rsn, GetTransactionLocation(
                                               /*type=*/type,
                                               /*incoming=*/false))));
  ASSIGN_OR_RETURN(PacketMetadata* const packet_metadata,
                   transaction->GetPacketMetadata(type));

  WindowTypeQueue queue_type;
  switch (type) {
    case falcon::PacketType::kPullRequest:
    case falcon::PacketType::kPushRequest:
      queue_type = WindowTypeQueue::kRequest;
      break;
    case falcon::PacketType::kPullData:
    case falcon::PacketType::kPushGrant:
    case falcon::PacketType::kPushSolicitedData:
    case falcon::PacketType::kPushUnsolicitedData:
      queue_type = WindowTypeQueue::kData;
      break;
    case falcon::PacketType::kAck:
      LOG(FATAL) << "ACKs should not go through retransmission scheduler.";
      break;
    case falcon::PacketType::kNack:
      LOG(FATAL) << "NACKs should not go through retransmission scheduler.";
      break;
    case falcon::PacketType::kBack:
      LOG(FATAL) << "BACKs should not go through retransmission scheduler.";
      break;
    case falcon::PacketType::kEack:
      LOG(FATAL) << "EACKs should not go through retransmission scheduler.";
      break;
    case falcon::PacketType::kResync:
      LOG(FATAL) << "Logic for handling resync packet not implemented.";
      break;
    case falcon::PacketType::kInvalid:
      LOG(FATAL) << "Invalid (phantom) request should not go through "
                    "retransmission scheduler.";
      break;
  }
  // Create work id that is going to be dequeued from the appropriate queue.
  RetransmissionWorkId work_id(rsn, packet_metadata->psn, type);
  connection_queues_[scid].Dequeue(queue_type, work_id);

  VLOG(2) << "Recompute after dequeue";
  RecomputeEligibility(scid);

  return absl::OkStatus();
}

// Performs one unit of work from the retransmission scheduler.
bool ProtocolRetransmissionScheduler::ScheduleWork() {
  VLOG(2) << "[Scheduler] Scheduling next work. ";
  CHECK_OK_THEN_ASSIGN(uint32_t scid, connection_policy_.GetNextEntity());
  WindowTypeQueue candidate_window_type_queue = next_window_type_[scid];

  RetransmissionWorkId work_id =
      connection_queues_[scid].Peek(candidate_window_type_queue);

  // Must meet criteria here or RecomputeEligibility is wrong.
  CHECK(CanSend(scid, work_id.psn, work_id.type));  // Crash OK.

  // Update next window type to be scheduled.
  if (candidate_window_type_queue == WindowTypeQueue::kRequest) {
    next_window_type_[scid] = WindowTypeQueue::kData;
  } else {
    next_window_type_[scid] = WindowTypeQueue::kRequest;
  }

  SchedulePacket(scid, candidate_window_type_queue, work_id);
  return true;
}

void ProtocolRetransmissionScheduler::RecomputeEligibility(uint32_t scid) {
  VLOG(2) << "[Scheduler] Recompute Eligibility " << scid << " "
          << absl::ToInt64Nanoseconds(
                 falcon_->get_environment()->ElapsedTime());
  if (!connection_queues_.contains(scid)) {
    return;
  }

  ConnectionStateManager* const conn_state_manager =
      falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       conn_state_manager->PerformDirectLookup(scid));

  bool meets_op_rate_criteria = (connection_state->is_op_rate_limited == 0);

  if (!meets_op_rate_criteria) {
    connection_policy_.MarkEntityInactive(scid);
    return;
  }

  bool has_request_work = false;
  bool has_data_work = false;

  // Check if Request queue has work eligible to be transmitted.
  if (!connection_queues_[scid].IsEmpty(WindowTypeQueue::kRequest)) {
    // Peek into the HoL queue entry.
    const RetransmissionWorkId& work_id =
        connection_queues_[scid].Peek(WindowTypeQueue::kRequest);
    if (falcon_->get_packet_reliability_manager()
            ->MeetsRetransmissionCCTxGatingCriteria(scid, work_id.psn,
                                                    work_id.type)) {
      has_request_work = true;
    }
  }

  // Check if Data queue has work eligible to be transmitted.
  if (!connection_queues_[scid].IsEmpty(WindowTypeQueue::kData)) {
    const RetransmissionWorkId& work_id =
        connection_queues_[scid].Peek(WindowTypeQueue::kData);
    if (falcon_->get_packet_reliability_manager()
            ->MeetsRetransmissionCCTxGatingCriteria(scid, work_id.psn,
                                                    work_id.type)) {
      has_data_work = true;
    }
  }

  // Mark the connection as active or inactive depending on eligible work.
  if (has_request_work || has_data_work) {
    connection_policy_.MarkEntityActive(scid);
    // Update next window type depending on which window has work.
    if (next_window_type_[scid] == WindowTypeQueue::kRequest &&
        !has_request_work) {
      next_window_type_[scid] = WindowTypeQueue::kData;
    }
    if (next_window_type_[scid] == WindowTypeQueue::kData && !has_data_work) {
      next_window_type_[scid] = WindowTypeQueue::kRequest;
    }
  } else {
    connection_policy_.MarkEntityInactive(scid);
  }

  if (connection_policy_.HasWork()) {
    falcon_->get_arbiter()->ScheduleSchedulerArbiter();
  }
}

bool ProtocolRetransmissionScheduler::CanSend(uint32_t scid, uint32_t psn,
                                              falcon::PacketType type) {
  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       state_manager->PerformDirectLookup(scid));
  bool meets_op_rate_criteria = (connection_state->is_op_rate_limited == 0);
  return (meets_op_rate_criteria &&
          falcon_->get_packet_reliability_manager()
              ->MeetsRetransmissionCCTxGatingCriteria(scid, psn, type));
}

// Returns true if the retransmission scheduler has outstanding work.
bool ProtocolRetransmissionScheduler::HasWork() {
  return falcon_->CanSendPacket() && connection_policy_.HasWork();
}

// Schedules the given packet by handing it off to the downstream blocks.
void ProtocolRetransmissionScheduler::SchedulePacket(
    uint32_t scid, WindowTypeQueue queue_type, RetransmissionWorkId& work_id) {
  VLOG(2) << "[" << falcon_->get_host_id() << ": "
          << falcon_->get_environment()->ElapsedTime() << "][" << scid << ", "
          << work_id.rsn << ", " << static_cast<int>(work_id.type) << "] "
          << "Packet in retransmission scheduler picked by arbiter.";
  falcon_->get_stats_manager()->UpdateSchedulerCounters(
      SchedulerTypes::kRetransmission, true);

  ConnectionStateManager* const state_manager = falcon_->get_state_manager();
  CHECK_OK_THEN_ASSIGN(auto connection_state,
                       state_manager->PerformDirectLookup(scid));
  CHECK_OK_THEN_ASSIGN(auto transaction,
                       connection_state->GetTransaction(TransactionKey(
                           work_id.rsn, GetTransactionLocation(
                                            /*type=*/work_id.type,
                                            /*incoming=*/false))));
  CHECK_OK_THEN_ASSIGN(auto packet_metadata,
                       transaction->GetPacketMetadata(work_id.type));

  falcon_->get_stats_manager()->UpdateRetxRsnSeries(
      scid, work_id.rsn, work_id.type, packet_metadata->retransmission_reason);

  packet_metadata->schedule_status = absl::OkStatus();
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

  CHECK_OK(falcon_->get_packet_reliability_manager()->TransmitPacket(
      scid, work_id.rsn, work_id.type));

  connection_queues_[scid].Dequeue(queue_type, work_id);

  VLOG(2) << "Recompute after schedule";
  RecomputeEligibility(scid);
}

}  // namespace isekai
