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

#include "isekai/host/rdma/rdma_falcon_work_scheduler.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "isekai/common/common_util.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/constants.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/token_bucket.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/rdma/rdma_base_model.h"
#include "isekai/host/rdma/rdma_component_interfaces.h"

namespace isekai {
namespace {

// Flag: enable_total_xoff
constexpr std::string_view kStatScalarTotalRequestXoffTime =
    "rdma.total_request_xoff_time_ns";
constexpr std::string_view kStatScalarTotalGlobalXoffTime =
    "rdma.total_global_xoff_time_ns";

// Flag: enable_credit_stall
constexpr std::string_view kStatScalarTotalRequestCreditStallTime =
    "rdma.total_request_credit_stall_time_ns";
constexpr std::string_view kStatScalarTotalResponseCreditStallTime =
    "rdma.total_response_credit_stall_time_ns";

}  // namespace

StatisticsCollectionConfig::RdmaFlags
    RdmaFalconRoundRobinWorkScheduler::stats_collection_flags_;

RdmaFalconRoundRobinWorkScheduler::RdmaFalconRoundRobinWorkScheduler(
    Environment* env, RdmaBaseModel<FalconQpContext>* rdma,
    const RdmaConfig& config, uint32_t mtu_size,
    RdmaQpManagerInterface* qp_manager,
    StatisticCollectionInterface* stats_collector)
    : request_xoff_(false),
      global_xoff_(false),
      is_idle_(true),
      current_work_type_(WorkType::kRequest),
      last_scheduler_run_time_(absl::ZeroDuration()),
      rdma_mtu_(mtu_size),
      env_(env),
      rdma_(rdma),
      config_(config),
      qp_manager_(qp_manager),
      scheduler_pipeline_delay_time_(
          absl::Nanoseconds(config.scheduler_pipeline_delay_in_cycles() *
                            config.chip_cycle_time_ns())),
      tx_token_bucket_refill_interval_(
          absl::Nanoseconds(config.tx_rate_limiter().refill_interval_ns())),
      tx_token_bucket_(
          config.tx_rate_limiter().burst_size_bytes() *
              absl::ToInt64Nanoseconds(absl::Seconds(1)) /
              absl::ToInt64Nanoseconds(tx_token_bucket_refill_interval_),
          tx_token_bucket_refill_interval_,
          // Bucket sized to accommodate at least an MTU RDMA packet + headers.
          config.tx_rate_limiter().burst_size_bytes() + rdma_mtu_ +
              kRdmaHeader),
      last_request_xoff_assert_time_(absl::ZeroDuration()),
      last_global_xoff_assert_time_(absl::ZeroDuration()),
      last_request_credit_stall_time_(absl::ZeroDuration()),
      last_response_credit_stall_time_(absl::ZeroDuration()),
      total_request_xoff_time_ns_(0),
      total_global_xoff_time_ns_(0),
      total_request_credit_stall_time_ns_(0),
      total_response_credit_stall_time_ns_(0) {
  remaining_quanta_bytes_[WorkType::kRequest] = 0;
  remaining_quanta_bytes_[WorkType::kResponse] = 0;
  remaining_quanta_ops_[WorkType::kRequest] = 0;
  remaining_quanta_ops_[WorkType::kResponse] = 0;
  remaining_global_credits_ = {
      .request_tx_packet = config.global_credits().tx_packet_request(),
      .request_tx_buffer = config.global_credits().tx_buffer_request(),
      .request_rx_packet = config.global_credits().rx_packet_request(),
      .request_rx_buffer = config.global_credits().rx_buffer_request(),
      .response_tx_packet = config.global_credits().tx_packet_data(),
      .response_tx_buffer = config.global_credits().tx_buffer_data(),
  };
  if (stats_collector && stats_collector->GetConfig().has_rdma_flags()) {
    stats_collection_flags_ = stats_collector->GetConfig().rdma_flags();
  } else {
    stats_collection_flags_ = DefaultConfigGenerator::DefaultRdmaStatsFlags();
  }
  if (config_.has_max_qp_oprate_million_per_sec()) {
    per_qp_inter_op_gap_ =
        absl::Microseconds(1) / config_.max_qp_oprate_million_per_sec();
  } else {
    per_qp_inter_op_gap_ =
        absl::Microseconds(1) / DefaultConfigGenerator::DefaultRdmaConfig()
                                    .max_qp_oprate_million_per_sec();
  }
}

void RdmaFalconRoundRobinWorkScheduler::AddQpToScheduleSet(QpId qp_id,
                                                           WorkType work_type) {
  FalconQpContext* context =
      down_cast<FalconQpContext*>(qp_manager_->DirectQpLookup(qp_id));

  // Check if we are trying to mark this qp active too soon, i.e., it is op-rate
  // limited.
  absl::Duration now = env_->ElapsedTime();
  if (context->next_schedule_time > now) {
    CHECK_OK(env_->ScheduleEvent(
        context->next_schedule_time - now,
        [this, qp_id, work_type]() { AddQpToScheduleSet(qp_id, work_type); }));
    return;
  }

  // Skip adding to active set if QP has insufficient credits.
  if (!HasCredits(context, work_type)) {
    return;
  }

  // Skip adding to active set if QP has been xoff'ed.
  if (work_type == WorkType::kRequest) {
    if (context->qp_request_xoff) {
      return;
    }
  } else if (work_type == WorkType::kResponse) {
    if (context->qp_response_xoff) {
      return;
    }
  }

  // Add to appropriate active list after checking for duplicates.
  if (work_type == WorkType::kRequest) {
    if (request_qp_set_.contains(qp_id)) {
      // WorkScheduler already contains SQ of qp_id.
      return;
    }
    request_wqs_.push_back(WorkQueue(qp_id, work_type));
    request_qp_set_.insert(qp_id);
  } else {
    if (response_qp_set_.contains(qp_id)) {
      // WorkScheduler already contains IIRQ of qp_id.
      return;
    }
    response_wqs_.push_back(WorkQueue(qp_id, work_type));
    response_qp_set_.insert(qp_id);
  }
  // If the scheduler is idle, immediately run it.
  if (is_idle_) {
    RunWorkScheduler();
  }
}

void RdmaFalconRoundRobinWorkScheduler::SetXoff(bool request_xoff,
                                                bool global_xoff) {
  // Track and update request/global xoff durations.
  if (stats_collection_flags_.enable_total_xoff()) {
    if (request_xoff_ != request_xoff) {
      if (request_xoff) {
        last_request_xoff_assert_time_ = env_->ElapsedTime();
      } else {
        total_request_xoff_time_ns_ += absl::ToInt64Nanoseconds(
            env_->ElapsedTime() - last_request_xoff_assert_time_);
      }
      rdma_->CollectScalarStats(kStatScalarTotalRequestXoffTime,
                                total_request_xoff_time_ns_);
    }
    if (global_xoff_ != global_xoff) {
      if (global_xoff) {
        last_global_xoff_assert_time_ = env_->ElapsedTime();
      } else {
        total_global_xoff_time_ns_ += absl::ToInt64Nanoseconds(
            env_->ElapsedTime() - last_global_xoff_assert_time_);
      }
      rdma_->CollectScalarStats(kStatScalarTotalGlobalXoffTime,
                                total_global_xoff_time_ns_);
    }
  }
  // Run the work scheduler if we were idle.
  request_xoff_ = request_xoff;
  global_xoff_ = global_xoff;
  if (is_idle_) {
    RunWorkScheduler();
  }
}

void RdmaFalconRoundRobinWorkScheduler::ReturnFalconCredits(
    const FalconCredit& credit) {
  remaining_global_credits_ += credit;
  UpdateCreditStallStats();
  // If scheduler was idle, run it in case the credits allow work to be done.
  if (is_idle_) {
    RunWorkScheduler();
  }
}

void RdmaFalconRoundRobinWorkScheduler::ConnectFalconInterface(
    FalconInterface* falcon) {
  falcon_ = falcon;
  if (falcon_ && falcon_->get_config() != nullptr) {
    falcon_rx_buffer_allocation_unit_ =
        falcon_->get_config()->rx_buffer_minimum_allocation_unit();
    falcon_tx_buffer_allocation_unit_ =
        falcon_->get_config()->tx_buffer_minimum_allocation_unit();
  } else {
    falcon_tx_buffer_allocation_unit_ = 64;
    falcon_rx_buffer_allocation_unit_ = 128;
  }

  // Falcon Specification: For ULP requests, txPmd varies from 144B to ~650B,
  // and txPktdata varies from 16B to 256B. For all received network packets,
  // rxPmd is up to 230B and rxPktdata is up to MTU size, note that rxPktDataLen
  // used in formula below accounts for BTH onwards only.
  minimum_request_credit_ = {
      .request_tx_packet = 1,
      .request_tx_buffer = CalculateFalconTxBufferCredits(
          /*txPktdataLen=*/256, /*txPmdSglLen=*/650,
          falcon_tx_buffer_allocation_unit_),
      .request_rx_packet = 1,
      .request_rx_buffer = CalculateFalconRxBufferCredits(
          /*length=*/rdma_mtu_, falcon_rx_buffer_allocation_unit_),
      .response_tx_packet = 0,
      .response_tx_buffer = 0,
  };
  minimum_response_credit_ = {
      .request_tx_packet = 0,
      .request_tx_buffer = 0,
      .request_rx_packet = 0,
      .request_rx_buffer = 0,
      .response_tx_packet = 1,
      .response_tx_buffer = CalculateFalconTxBufferCredits(
          /*txPktdataLen=*/256, /*txPmdSglLen=*/650,
          falcon_tx_buffer_allocation_unit_),
  };
}

inline bool RdmaFalconRoundRobinWorkScheduler::CanSendRequest() {
  return !(request_xoff_ || global_xoff_ || request_wqs_.empty()) &&
         minimum_request_credit_ <= remaining_global_credits_;
}

inline bool RdmaFalconRoundRobinWorkScheduler::CanSendResponse() {
  return !(global_xoff_ || response_wqs_.empty()) &&
         minimum_response_credit_ <= remaining_global_credits_;
}

void RdmaFalconRoundRobinWorkScheduler::RunWorkScheduler() {
  is_idle_ = false;
  // Check if we are trying to run the scheduler faster than the configured
  // chip_cycle_time. If yes, reschedule it at a later accurate time.
  absl::Duration now = env_->ElapsedTime();
  absl::Duration next_run_time =
      std::max(now, last_scheduler_run_time_ +
                        absl::Nanoseconds(config_.chip_cycle_time_ns()));
  if (next_run_time > now) {
    CHECK_OK(env_->ScheduleEvent(next_run_time - now,
                                 [this]() { RunWorkScheduler(); }));
    return;
  }

  // Check if we have enough TX rate limiter tokens to send an MTU transaction.
  // If not, we reschedule the work scheduler to run after a refill duration.
  bool sufficient_tokens = tx_token_bucket_.AreTokensAvailable(
      rdma_mtu_ + kRdmaHeader, env_->ElapsedTime());

  if (!sufficient_tokens) {
    // If there are not enough tokens, we first calculate the next refill time,
    // and then schedule the WorkScheduler to run after the tokens have been
    // replenished.
    absl::Duration next_tx_token_bucket_fill_time =
        tx_token_bucket_.NextRefillTime();

    // Since next fill time is calculated in absolute time units, subtract
    // current time from it when scheduling.
    CHECK_OK(env_->ScheduleEvent(next_tx_token_bucket_fill_time - now,
                                 [this]() { RunWorkScheduler(); }));
    return;
  }

  // Since we've passed scheduler recency and TX token rate limiter checks,
  // update credit stall stats.
  UpdateCreditStallStats();

  // Check if we can send out a request or response after checking for available
  // work, xoff status and global credits.
  bool can_send_request = CanSendRequest();
  bool can_send_response = CanSendResponse();

  if (!can_send_request && !can_send_response) {
    // Work scheduler doesn't have sufficient global credits remaining. It goes
    // to idle state and waits for credits to be replenished by Falcon before it
    // can be resumed.
    is_idle_ = true;
    return;
  }

  // Update current_work_type_ if we cannot send a request or response.
  if (current_work_type_ == WorkType::kResponse && !can_send_response) {
    current_work_type_ = WorkType::kRequest;
  } else if (current_work_type_ == WorkType::kRequest && !can_send_request) {
    current_work_type_ = WorkType::kResponse;
  }

  // Retrieve the WorkQueue at the head of the active list of current_work_type_
  // and schedule transactions from it. Replenish the quanta if this is a new
  // WorkQueue, which is indicated by both byte and op quanta being 0.
  QpId qp_id;
  if (remaining_quanta_bytes_[current_work_type_] == 0 &&
      remaining_quanta_ops_[current_work_type_] == 0) {
    remaining_quanta_bytes_[current_work_type_] =
        config_.work_scheduler_quanta();
    remaining_quanta_ops_[current_work_type_] = kMaxRdmaOpsPerQuanta;
  }

  if (current_work_type_ == WorkType::kRequest) {
    CHECK(!request_wqs_.empty())
        << "Trying to schedule Request transaction but no active SQs.";
    qp_id = request_wqs_.front().qp_id;
  } else {
    CHECK(!response_wqs_.empty())
        << "Trying to schedule Response transaction but no active IIRQs.";
    qp_id = response_wqs_.front().qp_id;
  }

  qp_manager_->InitiateQpLookup(
      qp_id, [this, qp_id](absl::StatusOr<BaseQpContext*> context) {
        CHECK(context.ok())
            << "Unknown QP id: " << qp_id << " " << context.status();
        SendTransaction(down_cast<FalconQpContext*>(context.value()));
      });
}

void RdmaFalconRoundRobinWorkScheduler::SendTransaction(
    FalconQpContext* context) {
  std::unique_ptr<Packet> packet;
  FalconCredit credit;
  uint32_t quanta_size;

  // Create the appropriate packet type.
  if (current_work_type_ == WorkType::kRequest) {
    const RdmaOp* op = context->send_queue.front().get();
    credit = ComputeRequestCredit(op);
    packet = CreateRequestPacket(context);
    quanta_size = GetRequestQuantaSize(packet.get());
  } else {
    CHECK(context->is_connected()) << "Read response works in RC mode only.";
    InboundReadRequest request =
        std::move(context->inbound_read_request_queue.front());
    credit = ComputeResponseCredit(request);
    packet = CreateResponsePacket(context);
    quanta_size = GetResponseQuantaSize(packet.get());
  }

  // Update QP context credits, global credits and quanta.
  context->credit_usage += credit;
  remaining_global_credits_ -= credit;
  // Increment outstanding outbound read requests.
  if (packet->rdma.opcode == Packet::Rdma::Opcode::kReadRequest) {
    ++context->outbound_read_requests;
  }

  // Use up the quanta according to the packet request/response size. Note that
  // the packet header size is not included.
  if (remaining_quanta_bytes_[current_work_type_] >= quanta_size) {
    remaining_quanta_bytes_[current_work_type_] -= quanta_size;
  } else {
    remaining_quanta_bytes_[current_work_type_] = 0;
  }
  --remaining_quanta_ops_[current_work_type_];

  // Consume tokens based on actual request size sent out. We are guaranteed to
  // have enough tokens since this function is scheduled after making the check.
  uint32_t tokens_consumed_bytes;
  if (packet->rdma.opcode == Packet::Rdma::Opcode::kReadRequest) {
    // For read requests, tokens consumed is just the RDMA headers.
    tokens_consumed_bytes = packet->rdma.data_length;
  } else {
    // For write requests and responses, tokens consumed is equal to RDMA
    // payload, which includes headers and data. Note for responses, the
    // request_length is a union with response_length and hence the same.
    tokens_consumed_bytes = packet->rdma.request_length;
  }
  tx_token_bucket_.RequestTokens(tokens_consumed_bytes);

  absl::Duration now = env_->ElapsedTime();
  // Update qp next_schedule_time according to configured op rate.
  if (context->next_schedule_time <= now) {
    context->next_schedule_time = now + per_qp_inter_op_gap_;
  } else {
    context->next_schedule_time += per_qp_inter_op_gap_;
  }

  // To emulate the delay between RDMA and Falcon, the RDMA scheduler sends
  // transactions to Falcon after chip_cycle_time * pipeline_delay_cycle.
  last_scheduler_run_time_ = now;
  CHECK_OK(env_->ScheduleEvent(scheduler_pipeline_delay_time_,
                               [this, p = packet.release()]() {
                                 auto p_ = std::unique_ptr<Packet>(p);
                                 falcon_->InitiateTransaction(std::move(p_));
                               }));

  // Check if we are over the ORD limit, and the next op is a read request.
  if (context->outbound_read_requests >= config_.outbound_read_queue_depth() &&
      !context->send_queue.empty() &&
      context->send_queue.front()->opcode == RdmaOpcode::kRead) {
    // Remove this work queue from active set.
    request_qp_set_.erase(context->qp_id);
    request_wqs_.pop_front();
    CHECK_OK(
        env_->ScheduleEvent(absl::Nanoseconds(config_.chip_cycle_time_ns()),
                            [this]() { RunWorkScheduler(); }));

    // Reset quanta for next WorkQueue, and update current_work_type_.
    remaining_quanta_bytes_[current_work_type_] = 0;
    remaining_quanta_ops_[current_work_type_] = 0;
    UpdateWorkType();
    return;
  }

  // Decide on next scheduler transition depending on remaining quanta and
  // credits available for the current WorkQueue.
  bool has_credits = HasCredits(context, current_work_type_);
  bool has_quanta = remaining_quanta_bytes_[current_work_type_] > 0 &&
                    remaining_quanta_ops_[current_work_type_] > 0;
  bool no_xoff =
      (current_work_type_ == WorkType::kRequest && !context->qp_request_xoff) ||
      (current_work_type_ == WorkType::kResponse && !context->qp_response_xoff);

  // Current WorkQueue has schedulable work and quanta, and the xoff is not
  // asserted, so schedule the next transaction on the same WorkQueue after
  // chip_cycle_time.
  if (has_quanta && has_credits && no_xoff) {
    // Return to the main scheduler loop for Falcon credit checking.
    CHECK_OK(
        env_->ScheduleEvent(absl::Nanoseconds(config_.chip_cycle_time_ns()),
                            [this]() { RunWorkScheduler(); }));
    return;
  }

  // Remove the WorkQueue from the active list.
  if (current_work_type_ == WorkType::kRequest) {
    request_wqs_.pop_front();
    request_qp_set_.erase(context->qp_id);
  } else {
    response_wqs_.pop_front();
    response_qp_set_.erase(context->qp_id);
  }

  // If QP has credits and xoff is not asserted, add it back to the active list
  // again at next_schedule_time.
  if (has_credits && no_xoff) {
    CHECK_OK(env_->ScheduleEvent(
        context->next_schedule_time - now, [this, qp_id = context->qp_id]() {
          AddQpToScheduleSet(qp_id, current_work_type_);
        }));
  }

  // Reset quanta for next WorkQueue, and update current_work_type_.
  remaining_quanta_bytes_[current_work_type_] = 0;
  remaining_quanta_ops_[current_work_type_] = 0;
  UpdateWorkType();

  // Schedule the next WorkQueue.
  CHECK_OK(env_->ScheduleEvent(absl::Nanoseconds(config_.chip_cycle_time_ns()),
                               [this]() { RunWorkScheduler(); }));
}

std::unique_ptr<Packet> RdmaFalconRoundRobinWorkScheduler::CreateRequestPacket(
    FalconQpContext* context) {
  auto packet = std::make_unique<Packet>();
  RdmaOp* op = context->send_queue.front().get();

  switch (op->opcode) {
    case RdmaOpcode::kRecv:
      LOG(FATAL) << "Cannot schedule recv op to be sent";
      break;
    case RdmaOpcode::kRead:
      packet->rdma.opcode = Packet::Rdma::Opcode::kReadRequest;
      packet->rdma.data_length = kReadHeaderSize;
      // This corresponds to the size of RDMA Response headers coming back.
      packet->rdma.request_length = kResponseHeaderSize;
      break;
    case RdmaOpcode::kSend:
      packet->rdma.opcode = Packet::Rdma::Opcode::kSendOnly;
      packet->rdma.data_length = kSendHeaderSize + op->inline_payload_length;
      packet->rdma.request_length = packet->rdma.data_length;
      break;
    case RdmaOpcode::kWrite:
      packet->rdma.opcode = Packet::Rdma::Opcode::kWriteOnly;
      packet->rdma.data_length = kWriteHeaderSize + op->inline_payload_length;
      packet->rdma.request_length = packet->rdma.data_length;
      break;
  }

  bool is_end_packet = false;
  if (op->inline_payload_length != 0) {
    CHECK(op->sgl.empty()) << "sgl and inline payload shouldn't coexist.";
    packet->rdma.inline_payload_length = op->inline_payload_length;
    is_end_packet = true;
  } else {
    packet->rdma.sgl = CutPacketFromSgl(&op->sgl, rdma_mtu_);
    is_end_packet = op->sgl.empty();

    // If an SGL is provided, we add its length to the total request_length.
    // For Read requests, this is the expected amount of data coming back. For
    // Send/Write requests, this is the total amount of data to target Falcon.
    packet->rdma.request_length += absl::c_accumulate(packet->rdma.sgl, 0);
    packet->metadata.sgl_length = GetSglLength(packet.get());
  }

  packet->metadata.rdma_src_qp_id = context->qp_id;
  packet->metadata.scid = context->scid;
  packet->rdma.dest_qp_id = op->dest_qp_id;
  packet->rdma.rsn = context->next_rsn++;

  if (!op->stats.is_scheduled) {
    op->stats.is_scheduled = true;
    op->stats.start_timestamp = env_->ElapsedTime();
    op->start_seq = packet->rdma.rsn;
  }

  // Update the map from RSN to RdmaOp.
  context->rsn_to_op.emplace(packet->rdma.rsn, op);

  // If this was the last packet of this op, place the completion handler into
  // the completion queue for it to be invoked later.
  if (is_end_packet) {
    packet->metadata.is_last_packet = true;
    op->stats.finish_timestamp = env_->ElapsedTime();
    op->end_seq = packet->rdma.rsn;
    op->n_seqs = op->end_seq - op->start_seq + 1;
    context->completion_queue.emplace(packet->rdma.rsn,
                                      std::move(context->send_queue.front()));
    context->send_queue.pop_front();
  }
  return packet;
}

std::unique_ptr<Packet> RdmaFalconRoundRobinWorkScheduler::CreateResponsePacket(
    FalconQpContext* context) {
  auto packet = std::make_unique<Packet>();
  InboundReadRequest request =
      std::move(context->inbound_read_request_queue.front());
  context->inbound_read_request_queue.pop_front();

  packet->metadata.rdma_src_qp_id = context->qp_id;
  packet->rdma.opcode = Packet::Rdma::Opcode::kReadResponseOnly;
  packet->rdma.sgl = std::move(request.sgl);
  packet->rdma.rsn = request.rsn;
  packet->metadata.scid = context->scid;
  packet->rdma.dest_qp_id = context->dest_qp_id;
  packet->rdma.data_length = kResponseHeaderSize;
  // This is the total size of the response being sent out.
  packet->rdma.response_length =
      kResponseHeaderSize + absl::c_accumulate(request.sgl, 0);
  packet->metadata.sgl_length = GetSglLength(packet.get());
  return packet;
}

int RdmaFalconRoundRobinWorkScheduler::GetSglLength(Packet* packet) {
  // The number of SGL entries is rounded up to closest power of 2, with a limit
  // of 32. Each SGL entry takes 16 bytes, plus a constant 8 bytes.
  int sgl_entries = packet->rdma.sgl.size();
  if (sgl_entries <= 1) {
    return kSglHeaderSize + 1 * kSglFragmentHeaderSize;
  } else if (sgl_entries <= 4) {
    return kSglHeaderSize + 4 * kSglFragmentHeaderSize;
  } else if (sgl_entries <= 8) {
    return kSglHeaderSize + 8 * kSglFragmentHeaderSize;
  } else if (sgl_entries <= 16) {
    return kSglHeaderSize + 16 * kSglFragmentHeaderSize;
  } else if (sgl_entries <= 32) {
    return kSglHeaderSize + 32 * kSglFragmentHeaderSize;
  } else {
    LOG(FATAL) << "SGL length exceeds the limit of 32 entries.";
  }
}

// Calculate quanta according to the packet request/response payload size.  The
// size of a packet on the wire is given by the RDMA payload (rdma.data_length)
// plus all other outer headers (Falcon, UDP, IP, Ethernet).
uint32_t RdmaFalconRoundRobinWorkScheduler::GetRequestQuantaSize(
    Packet* packet) {
  switch (packet->rdma.opcode) {
    case Packet::Rdma::Opcode::kReadRequest:
      return packet->rdma.data_length + kOuterRdmaHeaderSize;
    case Packet::Rdma::Opcode::kSendOnly:
    case Packet::Rdma::Opcode::kWriteOnly:
      return packet->rdma.request_length + kOuterRdmaHeaderSize;
    default:
      LOG(FATAL) << "Wrong packet op type";
  }
}

uint32_t RdmaFalconRoundRobinWorkScheduler::GetResponseQuantaSize(
    Packet* packet) {
  return packet->rdma.response_length + kOuterRdmaHeaderSize;
}

void RdmaFalconRoundRobinWorkScheduler::UpdateWorkType() {
  switch (current_work_type_) {
    case WorkType::kRequest:
      if (CanSendResponse()) {
        current_work_type_ = WorkType::kResponse;
      }
      break;
    case WorkType::kResponse:
      if (CanSendRequest()) {
        current_work_type_ = WorkType::kRequest;
      }
      break;
  }
}

std::vector<uint32_t> RdmaFalconRoundRobinWorkScheduler::CutPacketFromSgl(
    std::vector<uint32_t>* input, uint32_t mtu) {
  uint32_t accumulated_size = 0;
  std::vector<uint32_t> sgl;
  int sgl_idx = 0;
  while (sgl_idx < input->size() && accumulated_size < mtu) {
    if (accumulated_size + input->at(sgl_idx) <= mtu) {
      accumulated_size += input->at(sgl_idx);
      sgl.push_back(input->at(sgl_idx));
      sgl_idx++;
    } else {
      uint32_t delta = mtu - accumulated_size;
      accumulated_size += delta;
      input->at(sgl_idx) -= delta;
      sgl.push_back(delta);
    }
  }
  input->erase(input->begin(), input->begin() + sgl_idx);
  return sgl;
}

bool RdmaFalconRoundRobinWorkScheduler::HasCredits(FalconQpContext* context,
                                                   WorkType work_type) {
  FalconCredit credit;
  FalconCredit new_credit_usage = context->credit_usage;
  switch (work_type) {
    case WorkType::kRequest:
      if (context->send_queue.empty()) {
        return false;
      }
      credit = ComputeRequestCredit(context->send_queue.front().get());
      break;
    case WorkType::kResponse:
      if (context->inbound_read_request_queue.empty()) {
        return false;
      }
      credit =
          ComputeResponseCredit(context->inbound_read_request_queue.front());
      break;
  }

  new_credit_usage += credit;
  return new_credit_usage <= context->credit_limit;
}

FalconCredit RdmaFalconRoundRobinWorkScheduler::ComputeRequestCredit(
    const RdmaOp* op) {
  FalconCredit credit;
  switch (op->opcode) {
    case RdmaOpcode::kSend:
      credit.request_tx_packet = 1;
      credit.request_tx_buffer = CalculateFalconTxBufferCredits(
          kSendHeaderSize + op->inline_payload_length,
          op->sgl.size() * kSglFragmentHeaderSize,
          falcon_tx_buffer_allocation_unit_);
      // For the returning Ack.
      credit.request_rx_packet = 1;
      break;

    case RdmaOpcode::kWrite:
      credit.request_tx_packet = 1;
      credit.request_tx_buffer = CalculateFalconTxBufferCredits(
          kWriteHeaderSize + op->inline_payload_length,
          op->sgl.size() * kSglFragmentHeaderSize,
          falcon_tx_buffer_allocation_unit_);
      credit.request_rx_packet = 1;
      break;

    case RdmaOpcode::kRead:
      credit.request_tx_packet = 1;
      credit.request_tx_buffer = CalculateFalconTxBufferCredits(
          kReadHeaderSize, op->sgl.size() * kSglFragmentHeaderSize,
          falcon_tx_buffer_allocation_unit_);
      credit.request_rx_packet = 1;
      credit.request_rx_buffer = CalculateFalconRxBufferCredits(
          kResponseHeaderSize +
              std::min(rdma_mtu_, absl::c_accumulate(op->sgl, 0U)),
          falcon_rx_buffer_allocation_unit_);
      break;

    case RdmaOpcode::kRecv:
      LOG(FATAL) << "Cannot calculate credit for recv op to be sent";
  }
  return credit;
}

FalconCredit RdmaFalconRoundRobinWorkScheduler::ComputeResponseCredit(
    const InboundReadRequest& req) {
  FalconCredit credit;
  credit.response_tx_packet = 1;
  credit.response_tx_buffer = CalculateFalconTxBufferCredits(
      kResponseHeaderSize, req.sgl.size() * kSglFragmentHeaderSize,
      falcon_tx_buffer_allocation_unit_);
  return credit;
}

void RdmaFalconRoundRobinWorkScheduler::UpdateCreditStallStats() {
  if (!stats_collection_flags_.enable_credit_stall()) {
    return;
  }
  if (minimum_request_credit_ <= remaining_global_credits_ ||
      request_wqs_.empty()) {
    // Not stalled by request credits.
    if (last_request_credit_stall_time_ != absl::ZeroDuration()) {
      total_request_credit_stall_time_ns_ += absl::ToInt64Nanoseconds(
          env_->ElapsedTime() - last_request_credit_stall_time_);
      last_request_credit_stall_time_ = absl::ZeroDuration();
    }
  } else {
    // Stalled by request credits.
    if (last_request_credit_stall_time_ == absl::ZeroDuration()) {
      last_request_credit_stall_time_ = env_->ElapsedTime();
    }
  }

  if (minimum_response_credit_ <= remaining_global_credits_ ||
      response_wqs_.empty()) {
    // Not stalled by response credits.
    if (last_response_credit_stall_time_ != absl::ZeroDuration()) {
      total_response_credit_stall_time_ns_ += absl::ToInt64Nanoseconds(
          env_->ElapsedTime() - last_response_credit_stall_time_);
      last_response_credit_stall_time_ = absl::ZeroDuration();
    }
  } else {
    // Stalled by reponse credits.
    if (last_response_credit_stall_time_ == absl::ZeroDuration()) {
      last_response_credit_stall_time_ = env_->ElapsedTime();
    }
  }
  rdma_->CollectScalarStats(kStatScalarTotalRequestCreditStallTime,
                            total_request_credit_stall_time_ns_);
  rdma_->CollectScalarStats(kStatScalarTotalResponseCreditStallTime,
                            total_response_credit_stall_time_ns_);
}

}  // namespace isekai
