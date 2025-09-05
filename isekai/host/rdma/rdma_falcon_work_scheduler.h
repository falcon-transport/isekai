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

#ifndef ISEKAI_HOST_RDMA_RDMA_FALCON_WORK_SCHEDULER_H_
#define ISEKAI_HOST_RDMA_RDMA_FALCON_WORK_SCHEDULER_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/token_bucket.h"
#include "isekai/host/rdma/rdma_base_model.h"
#include "isekai/host/rdma/rdma_component_interfaces.h"

namespace isekai {

// A simple round-robin work scheduler that cycles over all active WorkQueues
// among all QPs and sends out a quantum of bytes for each QP every round. An
// active WorkQueue is define as one having pending ops *and* sufficient credits
// to send a transaction to Falcon. The scheduler keeps separate lists for
// Request and Response WQs and alternated between them (xoff permitting).
class RdmaFalconRoundRobinWorkScheduler : public RdmaWorkSchedulerInterface {
 public:
  RdmaFalconRoundRobinWorkScheduler(
      Environment* env, RdmaBaseModel<FalconQpContext>* rdma,
      const RdmaConfig& config, uint32_t mtu_size,
      RdmaQpManagerInterface* qp_manager,
      StatisticCollectionInterface* stats_collector);

  void AddQpToScheduleSet(QpId qp_id, WorkType work_type) override;
  void SetXoff(bool request_xoff, bool global_xoff) override;
  void ConnectFalconInterface(FalconInterface* falcon) override;

  // Schedules the next WorkQueue for processing and sending out a transaction.
  void RunWorkScheduler();

  // Returns the given Falcon credits back to the work scheduler, which uses it
  // to decide whether it can send any more transactions to Falcon or not.
  void ReturnFalconCredits(const FalconCredit& credit);

  // Computes the Falcon credits required to send a request operation.
  FalconCredit ComputeRequestCredit(const RdmaOp* op);

  // Computes the Falcon credit required to send a read response.
  FalconCredit ComputeResponseCredit(const InboundReadRequest& req);

  // Creates a packet given an input scatter-gather-list and an MTU. It trims
  // the SGL after removing a packet from its head.
  std::vector<uint32_t> CutPacketFromSgl(std::vector<uint32_t>* input,
                                         uint32_t mtu);

  // Checks whether the QueuePair context has pending ops and sufficient credits
  // to send a transaction to Falcon of the given WorkType.
  bool HasCredits(FalconQpContext* context, WorkType work_type);

 private:
  // Checks whether the scheduler can send out a request or response.
  inline bool CanSendRequest();
  inline bool CanSendResponse();
  // Sends a single transaction to Falcon from the head of the active list.
  void SendTransaction(FalconQpContext* context);

  // Calculate the required quanta using request/response_size - header_size.
  uint32_t GetRequestQuantaSize(Packet* packet);
  uint32_t GetResponseQuantaSize(Packet* packet);

  // Updates the WorkType for the next time the scheduler has to pick a new
  // WorkQueue.
  std::unique_ptr<Packet> CreateRequestPacket(FalconQpContext* context);
  std::unique_ptr<Packet> CreateResponsePacket(FalconQpContext* context);
  int GetSglLength(Packet* packet);
  void UpdateWorkType();

  // Tracks and updates the time RDMA requests and responses were stalled due to
  // insufficient Falcon credits.
  void UpdateCreditStallStats();

  // List of active WorkQueues that have a pending op on either the SQ or IIRQ
  // and sufficient credits to send a transaction to Falcon. These list maintain
  // the round robin order of processing.
  std::deque<WorkQueue> request_wqs_;
  std::deque<WorkQueue> response_wqs_;

  // Set of active QPs that have request or response. We keep this (redundant
  // copy) to quickly check for duplicates while scheduling.
  absl::flat_hash_set<QpId> request_qp_set_;
  absl::flat_hash_set<QpId> response_qp_set_;

  // When request_xoff is set to true by Falcon, the work scheduler must stop
  // sending requests to Falcon, but can send responses.
  bool request_xoff_;
  // When global_xoff is set to true by Falcon, the work scheduler must stop
  // sending both requests and responses to Falcon.
  bool global_xoff_;
  // Keeps track of whether the work scheduler is processing a WorkQueue and
  // sending a packet to Falcon or sitting idle.
  bool is_idle_;
  // Remaining quanta (in terms of both bytes and ops) the current active
  // WorkQueue is allowed to send before we cycle to next WorkQueue. We keep
  // separate quanta for request and response.
  absl::flat_hash_map<WorkType, int> remaining_quanta_bytes_;
  absl::flat_hash_map<WorkType, int> remaining_quanta_ops_;
  // The type of WorkQueue (request or response) the scheduler is processing
  // at the moment. This alternates between Request and Response.
  WorkType current_work_type_;
  // Last time when the work scheduler ran and sent out a transaction.
  absl::Duration last_scheduler_run_time_;

  uint32_t rdma_mtu_;

  // Total remaining global Falcon credits available for sending transactions.
  // Everytime a transaction is sent to Falcon (on any QueuePair/connection), we
  // deduct it from this pool. If there aren't sufficient credits, we don't send
  // the transaction and wait for credits to be returned.
  FalconCredit remaining_global_credits_;
  // The minimum amount of Falcon credits required to send an MTU sized request
  // or response to Falcon. The WorkScheduler goes to idle if the remaining
  // credits falls below these thresholds. It restarts when credits are returned
  // by Falcon.
  FalconCredit minimum_request_credit_;
  FalconCredit minimum_response_credit_;

  Environment* const env_;
  RdmaBaseModel<FalconQpContext>* const rdma_;
  const RdmaConfig& config_;
  RdmaQpManagerInterface* const qp_manager_;
  FalconInterface* falcon_ = nullptr;
  // Constants for determining how many credits to reserve for transactions to
  // Falcon. We keep a local copy to avoid looking up falcon_->config everytime
  // we send a transaction to Falcon.
  uint32_t falcon_tx_buffer_allocation_unit_;
  uint32_t falcon_rx_buffer_allocation_unit_;

  // Constant for RDMA scheduler pipeline delay time. It is calculated by
  // chip_cycle_time_ns * scheduler_pipeline_delay_in_cycles.
  absl::Duration scheduler_pipeline_delay_time_;
  // Per-QP inter-op gap assuming max per-qp op-rate in the config.
  absl::Duration per_qp_inter_op_gap_;

  // A TX token bucket filter that models GRL in hardware.
  absl::Duration tx_token_bucket_refill_interval_;
  TokenBucket tx_token_bucket_;

  // Variables to keep track to total request_xoff, global_xoff time and RDMA
  // request/response stall due to insufficient credits.
  absl::Duration last_request_xoff_assert_time_;
  absl::Duration last_global_xoff_assert_time_;
  absl::Duration last_request_credit_stall_time_;
  absl::Duration last_response_credit_stall_time_;
  uint32_t total_request_xoff_time_ns_;
  uint32_t total_global_xoff_time_ns_;
  uint32_t total_request_credit_stall_time_ns_;
  uint32_t total_response_credit_stall_time_ns_;

  static StatisticsCollectionConfig::RdmaFlags stats_collection_flags_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_RDMA_RDMA_FALCON_WORK_SCHEDULER_H_
