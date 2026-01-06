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

#include "isekai/host/rdma/rdma_falcon_model.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/strings/substitute.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "isekai/common/common_util.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/tdigest.h"
#include "isekai/host/rdma/rdma_base_model.h"
#include "isekai/host/rdma/rdma_component_interfaces.h"
#include "isekai/host/rdma/rdma_falcon_work_scheduler.h"
#include "isekai/host/rdma/rdma_latency_histograms.pb.h"
#include "isekai/host/rdma/rdma_per_host_rx_buffers.h"
#include "isekai/host/rdma/rdma_qp_manager.h"
#include "isekai/host/rnic/memory_interface.h"

namespace isekai {
namespace {

// Flag: enable_op_timeseries
constexpr std::string_view kStatVectorOpLength =
    "rdma.op_length.qp$0.cid$1.op$2";
constexpr std::string_view kStatVectorOpStartRsn =
    "rdma.op_start_rsn.qp$0.cid$1.op$2";
constexpr std::string_view kStatVectorOpEndRsn =
    "rdma.op_end_rsn.qp$0.cid$1.op$2";
constexpr std::string_view kStatVectorOpPostTimestamp =
    "rdma.op_post_timestamp_ns.qp$0.cid$1.op$2";
constexpr std::string_view kStatVectorOpStartTimestamp =
    "rdma.op_start_timestamp_ns.qp$0.cid$1.op$2";
constexpr std::string_view kStatVectorOpFinishTimestamp =
    "rdma.op_finish_timestamp_ns.qp$0.cid$1.op$2";
constexpr std::string_view kStatVectorOpCompletionTimestamp =
    "rdma.op_completion_timestamp_ns.qp$0.cid$1.op$2";
constexpr std::string_view kStatVectorOpTotalLatency =
    "rdma.total_latency_ns.qp$0.cid$1.op$2";
constexpr std::string_view kStatVectorOpTransportLatency =
    "rdma.transport_latency_ns.qp$0.cid$1.op$2";

// Flag: enable_per_qp_xoff
constexpr std::string_view kStatScalarQpRequestXoffTime =
    "rdma.qp$0.cid$1.request_xoff_time_ns";
constexpr std::string_view kStatScalarQpResponseXoffTime =
    "rdma.qp$0.cid$1.response_xoff_time_ns";

}  // namespace

RdmaFalconModel::RdmaFalconModel(const RdmaConfig& config, Environment* env,
                                 StatisticCollectionInterface* stats_collector,
                                 ConnectionManagerInterface* connection_manager)
    : RdmaBaseModel<FalconQpContext>(RdmaType::kFalconRdma, config, env,
                                     stats_collector, connection_manager),
      work_scheduler_(env, this, config_, rdma_mtu_, &qp_manager_,
                      stats_collector),
      op_total_latency_(TDigest::New(kTdigestCompression)),
      op_transport_latency_(TDigest::New(kTdigestCompression)),
      op_queueing_latency_(TDigest::New(kTdigestCompression)) {
  if (config_.has_rnr_timeout_us()) {
    rnr_timeout_ = absl::Microseconds(config_.rnr_timeout_us());
  }
  if (config_.has_write_random_rnr_probability()) {
    write_random_rnr_probability_ = config_.write_random_rnr_probability();
    if (write_random_rnr_probability_ > 1.0 ||
        write_random_rnr_probability_ < 0.0)
      LOG(FATAL) << "write_random_rnr_probability must be in [0, 1].";
  }
  if (config_.has_read_random_rnr_probability()) {
    read_random_rnr_probability_ = config_.read_random_rnr_probability();
    if (read_random_rnr_probability_ > 1.0 ||
        read_random_rnr_probability_ < 0.0)
      LOG(FATAL) << "read_random_rnr_probability must be in [0, 1].";
  }
}

void RdmaFalconModel::CreateRcQp(QpId local_qp_id, QpId remote_qp_id,
                                 QpOptions& options,
                                 RdmaConnectedMode rc_mode) {
  // Create the qp context, and initialize it.
  auto context = std::make_unique<FalconQpContext>();
  InitializeQpContext(context.get(), local_qp_id, options);
  // Add the context to the qp manager.
  qp_manager_.CreateQp(std::move(context));
  qp_manager_.ConnectQp(local_qp_id, remote_qp_id, rc_mode);

  falcon_->SetupNewQp(options.scid, local_qp_id, QpType::kRC,
                      rc_mode == RdmaConnectedMode::kOrderedRc
                          ? OrderingMode::kOrdered
                          : OrderingMode::kUnordered);
}

void RdmaFalconModel::InitializeQpContext(BaseQpContext* base_context,
                                          QpId local_qp_id,
                                          QpOptions& options) {
  RdmaBaseModel::InitializeQpContext(base_context, local_qp_id, options);
  auto context = down_cast<FalconQpContext*>(base_context);
  context->scid = options.scid;
  context->rnr_timeout = rnr_timeout_;
  if (options.initial_credit.IsInitialized()) {
    context->credit_usage = options.initial_credit;
    context->credit_limit = options.initial_credit;
  } else {
    context->credit_limit = FalconCredit{
        .request_tx_packet = config_.per_qp_credits().tx_packet_request(),
        .request_tx_buffer = config_.per_qp_credits().tx_buffer_request(),
        .request_rx_packet = config_.per_qp_credits().rx_packet_request(),
        .request_rx_buffer = config_.per_qp_credits().rx_buffer_request(),
        .response_tx_packet = config_.per_qp_credits().tx_packet_data(),
        .response_tx_buffer = config_.per_qp_credits().tx_buffer_data(),
    };
  }
}

void RdmaFalconModel::ConnectFalcon(FalconInterface* falcon) {
  falcon_ = falcon;
  work_scheduler_.ConnectFalconInterface(falcon_);
}

void RdmaFalconModel::ConnectHostInterface(
    std::vector<std::unique_ptr<MemoryInterface>>* host_interface) {
  CHECK(config_.has_rx_buffer_config())
      << "Per Host rx buffer config is missing.";
  rdma_per_host_rx_buffers_ = std::make_unique<RdmaPerHostRxBuffers>(
      config_.rx_buffer_config(), host_interface, falcon_);
}

void RdmaFalconModel::HandleRxTransaction(
    std::unique_ptr<Packet> packet, std::unique_ptr<OpaqueCookie> cookie) {
  uint32_t qp_id = packet->rdma.dest_qp_id;
  qp_manager_.InitiateQpLookup(
      qp_id, [this, packet = std::move(packet), cookie = std::move(cookie)](
                 absl::StatusOr<BaseQpContext*> qp_context) mutable {
        CHECK_OK(qp_context.status())
            << "Received packet destined for unknown qp id: "
            << qp_context.status() << "\n"
            << packet->DebugString();
        pipeline_.emplace();
        pipeline_.back().is_completion = false;
        pipeline_.back().packet = std::move(packet);
        pipeline_.back().cookie = std::move(cookie);
        pipeline_.back().context =
            down_cast<FalconQpContext*>(qp_context.value());
        if (!pipeline_active_) {
          pipeline_active_ = true;
          PipelineDequeue();
        }
      });
}

void RdmaFalconModel::PostOp(FalconQpContext* context, RdmaOp op) {
  op.stats.post_timestamp = env_->ElapsedTime();
  RdmaBaseModel::PostOp(context, op);
  bool no_xoff = !(context->qp_request_xoff);
  if (context->send_queue.size() == 1 && no_xoff &&
      work_scheduler_.HasCredits(context, WorkType::kRequest)) {
    work_scheduler_.AddQpToScheduleSet(context->qp_id, WorkType::kRequest);
  }
}

void RdmaFalconModel::PipelineDequeue() {
  // Return to idle if pipeline is empty.
  if (pipeline_.empty()) {
    pipeline_active_ = false;
    return;
  }
  // If trying to run faster than expected, schedule for later.
  absl::Duration now = env_->ElapsedTime();
  absl::Duration next_run_time =
      pipeline_last_run_time + absl::Nanoseconds(config_.chip_cycle_time_ns());
  if (now < next_run_time) {
    CHECK_OK(env_->ScheduleEvent(next_run_time - now,
                                 [this] { PipelineDequeue(); }));
    return;
  }
  // Update last_run_time and do work.
  pipeline_last_run_time = now;
  auto context = pipeline_.front().context;
  if (pipeline_.front().is_completion) {
    ProcessCompletion(context, pipeline_.front().rsn,
                      pipeline_.front().syndrome,
                      pipeline_.front().destination_bifurcation_id);
  } else {
    ProcessRxTransaction(context, std::move(pipeline_.front().packet),
                         std::move(pipeline_.front().cookie));
  }
  pipeline_.pop();
  // Run the pipeline after chip_cycle_delay.
  CHECK_OK(env_->ScheduleEvent(absl::Nanoseconds(config_.chip_cycle_time_ns()),
                               [this] { PipelineDequeue(); }));
}

void RdmaFalconModel::ProcessRxTransaction(
    FalconQpContext* context, std::unique_ptr<Packet> packet,
    std::unique_ptr<OpaqueCookie> cookie) {
  switch (packet->rdma.opcode) {
    case Packet::Rdma::Opcode::kInvalid:
    case Packet::Rdma::Opcode::kAck:
      LOG(FATAL) << "Invalid opcode in packet\n" << packet->DebugString();
      break;
    case Packet::Rdma::Opcode::kSendOnly:
      ProcessIncomingSend(context, std::move(packet), std::move(cookie));
      break;
    // Packets below are only available in RC mode.
    case Packet::Rdma::Opcode::kWriteOnly:
      ProcessIncomingWrite(context, std::move(packet), std::move(cookie));
      break;
    case Packet::Rdma::Opcode::kReadRequest:
      ProcessIncomingReadRequest(context, std::move(packet), std::move(cookie));
      break;
    case Packet::Rdma::Opcode::kReadResponseOnly:
      ProcessIncomingReadResponse(context, std::move(packet),
                                  std::move(cookie));
      break;
  }
}

void RdmaFalconModel::ProcessIncomingSend(
    FalconQpContext* context, std::unique_ptr<Packet> packet,
    std::unique_ptr<OpaqueCookie> cookie) {
  CHECK(!context->is_connected() ||
        context->rc_mode == RdmaConnectedMode::kOrderedRc)
      << "UnorderedRC not implemented for Send.";

  uint32_t scid = context->scid;
  uint32_t rsn = packet->rdma.rsn;
  bool recv_mismatch = false;
  bool is_end = false;
  if (context->receive_queue.empty() || !IsRsnAcceptable(context, rsn)) {
    // An incoming Send arrived without a pending Recv, or RSN is not
    // acceptable. We must NACK-RNR in this condition.
    recv_mismatch = true;
  } else if (packet->rdma.inline_payload_length != 0) {
    is_end = true;
    uint32_t expected_payload_length = 0;
    for (uint32_t segment_length : context->receive_queue.front()->sgl) {
      expected_payload_length += segment_length;
    }
    if (expected_payload_length != packet->rdma.inline_payload_length) {
      // If the posted Recv buffer doesn't match the packet payload length, we
      // mismatch and NACK the packet.
      is_end = false;
      recv_mismatch = true;
    }
  } else {
    auto old_sgl = std::vector(context->receive_queue.front()->sgl);
    auto expected_sgl = work_scheduler_.CutPacketFromSgl(
        &context->receive_queue.front()->sgl, rdma_mtu_);
    is_end = context->receive_queue.front()->sgl.empty();
    if (packet->rdma.sgl != expected_sgl) {
      is_end = false;
      recv_mismatch = true;
      context->receive_queue.front()->sgl = std::move(old_sgl);
    }
  }

  if (recv_mismatch) {
    EnterRnrState(context);
    // If there is a mismatch, NACK the transaction with RNR.
    CHECK_OK(env_->ScheduleEvent(
        absl::Nanoseconds(config_.ack_nack_latency_ns()),
        [this, scid, rsn, context, cookie = std::move(cookie)]() mutable {
          falcon_->AckTransaction(scid, rsn, Packet::Syndrome::kRnrNak,
                                  context->rnr_timeout, std::move(cookie));
        }));
  } else {
    UpdateNextRsnToReceive(context);
    LeaveRnrState(context);

    CompletionCallback callback = nullptr;
    uint8_t host_index = packet->metadata.destination_bifurcation_id;
    // If all packets corresponding to a SEND/RECV RDMA op are received, get a
    // handle on the receive completion callback.
    if (is_end) {
      callback = std::move(context->receive_queue.front()->completion_callback);
      context->receive_queue.pop_front();
    }
    absl::Duration ack_time =
        env_->ElapsedTime() + absl::Nanoseconds(config_.ack_nack_latency_ns());
    // Ensure that the packet is written over PCIe, and then trigger ACK to
    // Falcon, and also trigger receive completion call back if all packets
    // corresponding to SEND/RECV op is received.
    rdma_per_host_rx_buffers_->Push(
        host_index, packet->falcon.payload_length,
        /*pcie_issue_callback=*/
        [this, ack_time, scid, rsn, cookie = std::move(cookie),
         completion_callback = std::move(callback), is_end]() mutable {
          absl::Duration now = env_->ElapsedTime();
          if (now < ack_time) {
            CHECK_OK(env_->ScheduleEvent(
                ack_time - now,
                [this, scid, rsn, cookie = std::move(cookie),
                 completion_callback = std::move(completion_callback),
                 is_end]() mutable {
                  falcon_->AckTransaction(scid, rsn, Packet::Syndrome::kAck,
                                          absl::ZeroDuration(),
                                          std::move(cookie));
                  if (is_end) {
                    completion_callback(Packet::Syndrome::kAck);
                  }
                }));
          } else {
            falcon_->AckTransaction(scid, rsn, Packet::Syndrome::kAck,
                                    absl::ZeroDuration(), std::move(cookie));
            if (is_end) {
              completion_callback(Packet::Syndrome::kAck);
            }
          }
        },
        /*pcie_complete_callback=*/nullptr);
  }
}

void RdmaFalconModel::ProcessIncomingWrite(
    FalconQpContext* context, std::unique_ptr<Packet> packet,
    std::unique_ptr<OpaqueCookie> cookie) {
  uint32_t scid = context->scid;
  uint32_t rsn = packet->rdma.rsn;

  if (!IsRsnAcceptable(context, rsn) || ShouldRandomRnrNackWrite()) {
    VLOG(2) << "[" << falcon_->get_host_id() << ": "
            << falcon_->get_environment()->ElapsedTime() << " RDMA][" << scid
            << ", " << rsn << ", write] " << "RDMA RNR-NACKs randomly";
    EnterRnrState(context);
    // Send RNR NACK.
    CHECK_OK(env_->ScheduleEvent(
        absl::Nanoseconds(config_.ack_nack_latency_ns()),
        [this, scid, rsn, context, cookie = std::move(cookie)]() mutable {
          falcon_->AckTransaction(scid, rsn, Packet::Syndrome::kRnrNak,
                                  context->rnr_timeout, std::move(cookie));
        }));
    return;
  }

  UpdateNextRsnToReceive(context);
  LeaveRnrState(context);

  // Enqueue packet in host buffer and schedule ack and completion when the
  // packet is transferred to the host over pcie.
  uint8_t host_index = packet->metadata.destination_bifurcation_id;
  absl::Duration ack_time =
      env_->ElapsedTime() + absl::Nanoseconds(config_.ack_nack_latency_ns());
  rdma_per_host_rx_buffers_->Push(
      host_index, packet->falcon.payload_length,
      /*pcie_issue_callback=*/
      [this, ack_time, scid, rsn, cookie = std::move(cookie)]() mutable {
        absl::Duration now = env_->ElapsedTime();
        if (now < ack_time) {
          CHECK_OK(env_->ScheduleEvent(
              ack_time - now,
              [this, scid, rsn, cookie = std::move(cookie)]() mutable {
                falcon_->AckTransaction(scid, rsn, Packet::Syndrome::kAck,
                                        absl::ZeroDuration(),
                                        std::move(cookie));
              }));
        } else {
          falcon_->AckTransaction(scid, rsn, Packet::Syndrome::kAck,
                                  absl::ZeroDuration(), std::move(cookie));
        }
      },
      /*pcie_complete_callback=*/nullptr);
}

void RdmaFalconModel::ProcessIncomingReadRequest(
    FalconQpContext* context, std::unique_ptr<Packet> packet,
    std::unique_ptr<OpaqueCookie> cookie) {
  // Note that we don't need to do any PCIe reads here, since the packet builder
  // will assemble the response data farther down in the TX path.
  if (!IsRsnAcceptable(context, packet->rdma.rsn) ||
      ShouldRandomRnrNackRead()) {
    auto scid = context->scid;
    auto rsn = packet->rdma.rsn;
    EnterRnrState(context);
    // Send RNR NACK.
    CHECK_OK(env_->ScheduleEvent(
        absl::Nanoseconds(config_.ack_nack_latency_ns()),
        [this, scid, rsn, context, cookie = std::move(cookie)]() mutable {
          falcon_->AckTransaction(scid, rsn, Packet::Syndrome::kRnrNak,
                                  context->rnr_timeout, std::move(cookie));
        }));
    return;
  }

  uint32_t scid = context->scid;
  uint32_t rsn = packet->rdma.rsn;
  if (context->inbound_read_request_queue.size() >=
      config_.inbound_read_queue_depth()) {
    EnterRnrState(context);
    // IRRQ is above the IRD limit. We must RNR-NACK the transaction to Falcon.
    CHECK_OK(env_->ScheduleEvent(
        absl::Nanoseconds(config_.ack_nack_latency_ns()),
        [this, scid, rsn, context, cookie = std::move(cookie)]() mutable {
          falcon_->AckTransaction(scid, rsn, Packet::Syndrome::kRnrNak,
                                  context->rnr_timeout, std::move(cookie));
        }));
    return;
  }

  UpdateNextRsnToReceive(context);
  LeaveRnrState(context);

  // Enqueue packet in the host buffer and schedule the ack + response when the
  // request is handled.
  // Note, a read request does not interact with the target host PCIe. However,
  // to ensure RSN ordering of ACKs to Falcon for incoming read/write requests,
  // we serialize them through the host interface. This is required to maintain
  // RDMA semantics of ordering between reads and writes.
  uint8_t host_index = packet->metadata.destination_bifurcation_id;
  uint32_t payload_length = packet->falcon.payload_length;
  absl::Duration ack_time =
      env_->ElapsedTime() + absl::Nanoseconds(config_.ack_nack_latency_ns());

  rdma_per_host_rx_buffers_->Push(
      host_index, payload_length,
      /*pcie_issue_callback=*/
      [this, context, cookie = std::move(cookie), packet = std::move(packet),
       ack_time]() mutable {
        absl::Duration now = env_->ElapsedTime();
        if (now >= ack_time) {
          ack_time = now;
        }
        CHECK_OK(env_->ScheduleEvent(
            ack_time - now, [this, context, packet = std::move(packet),
                             cookie = std::move(cookie)]() mutable {
              // Ack transaction to Falcon.
              falcon_->AckTransaction(context->scid, packet->rdma.rsn,
                                      Packet::Syndrome::kAck,
                                      absl::ZeroDuration(), std::move(cookie));

              // Schedule the read response to be enqueued into the scheduler
              // after response processing latency.
              CHECK_OK(env_->ScheduleEvent(
                  absl::Nanoseconds(config_.response_latency_ns()),
                  [this, context, packet = std::move(packet),
                   cookie = std::move(cookie)]() mutable {
                    // Enqueue the request.
                    context->inbound_read_request_queue.emplace_back(
                        packet->rdma.rsn, std::move(packet->rdma.sgl));
                    bool no_xoff = !(context->qp_response_xoff);
                    // If we enqueued the incoming op into an empty queue,
                    // current size of IRRQ will be 1, therefore notify the work
                    // scheduler to make it active.
                    if (context->inbound_read_request_queue.size() == 1 &&
                        no_xoff &&
                        work_scheduler_.HasCredits(context,
                                                   WorkType::kResponse)) {
                      work_scheduler_.AddQpToScheduleSet(context->qp_id,
                                                         WorkType::kResponse);
                    }
                  }));
            }));
      },
      /*pcie_complete_callback*/ nullptr);
}

void RdmaFalconModel::ProcessIncomingReadResponse(
    FalconQpContext* context, std::unique_ptr<Packet> packet,
    std::unique_ptr<OpaqueCookie> cookie) {
  // A ReadResponse or an ACK can never be NACKed by RDMA in Falcon.
  auto it = context->rsn_to_op.find(packet->rdma.rsn);
  CHECK(it != context->rsn_to_op.end());
  RdmaOp* rdma_op = it->second;
  rdma_op->n_seqs_finished++;
  // If n_seqs > 0 (i.e., the last request of this op has been sent), and
  // finished sequences == n_seqs, do completion.
  if (rdma_op->n_seqs > 0 && rdma_op->n_seqs_finished == rdma_op->n_seqs) {
    CompletionCallback completion_callback =
        std::move(rdma_op->completion_callback);
    CollectOpStats(context, rdma_op->end_seq);
    context->completion_queue.erase(rdma_op->end_seq);
    --context->outbound_read_requests;
    // If this read response brings us below the ORD limit, add the QP back to
    // the active set.
    if (context->outbound_read_requests < config_.outbound_read_queue_depth() &&
        !context->send_queue.empty() &&
        context->send_queue.front()->opcode == RdmaOpcode::kRead) {
      work_scheduler_.AddQpToScheduleSet(context->qp_id, WorkType::kRequest);
    }
    // Enqueue response into host_buffer for transfer to host and perform
    // callback completion once the transfer is finished.
    uint8_t host_index = packet->metadata.destination_bifurcation_id;
    rdma_per_host_rx_buffers_->Push(
        host_index, packet->falcon.payload_length,
        /*pcie_issue_callback=*/nullptr,
        /*pcie_complete_callback=*/[completion_callback]() {
          completion_callback(Packet::Syndrome::kAck);
        });
  } else {
    uint8_t host_index = packet->metadata.destination_bifurcation_id;
    rdma_per_host_rx_buffers_->Push(host_index, packet->falcon.payload_length,
                                    /*pcie_issue_callback=*/nullptr,
                                    /*pcie_complete_callback=*/nullptr);
  }
  // Erase this mapping from RSN to op.
  context->rsn_to_op.erase(it);
}

void RdmaFalconModel::IncreaseFalconCreditLimitForTesting(
    QpId qp_id, const FalconCredit& credit) {
  BaseQpContext* ret = qp_manager_.DirectQpLookup(qp_id);
  auto context = down_cast<FalconQpContext*>(ret);
  context->credit_limit += credit;

  bool no_request_xoff = !(context->qp_request_xoff);
  bool no_response_xoff = !(context->qp_response_xoff);

  // If these credits make the QP schedulable, notify the work scheduler.
  if (!context->send_queue.empty() && no_request_xoff &&
      work_scheduler_.HasCredits(context, WorkType::kRequest)) {
    work_scheduler_.AddQpToScheduleSet(qp_id, WorkType::kRequest);
  }
  if (!context->inbound_read_request_queue.empty() && no_response_xoff &&
      work_scheduler_.HasCredits(context, WorkType::kResponse)) {
    work_scheduler_.AddQpToScheduleSet(qp_id, WorkType::kResponse);
  }
  work_scheduler_.ReturnFalconCredits(credit);
}

void RdmaFalconModel::ReturnFalconCredit(QpId qp_id,
                                         const FalconCredit& credit) {
  BaseQpContext* ret = qp_manager_.DirectQpLookup(qp_id);
  auto context = down_cast<FalconQpContext*>(ret);

  if (!(credit <= context->credit_usage))
    LOG(FATAL) << "The returned credit should not be larger than the current "
                  "credit usage.";

  context->credit_usage -= credit;

  bool no_request_xoff = !(context->qp_request_xoff);
  bool no_response_xoff = !(context->qp_response_xoff);

  // If these credits make the QP schedulable, notify the work scheduler.
  if (!context->send_queue.empty() && no_request_xoff &&
      work_scheduler_.HasCredits(context, WorkType::kRequest)) {
    work_scheduler_.AddQpToScheduleSet(qp_id, WorkType::kRequest);
  }
  if (!context->inbound_read_request_queue.empty() && no_response_xoff &&
      work_scheduler_.HasCredits(context, WorkType::kResponse)) {
    work_scheduler_.AddQpToScheduleSet(qp_id, WorkType::kResponse);
  }
  work_scheduler_.ReturnFalconCredits(credit);
}

void RdmaFalconModel::SetXoff(bool request_xoff, bool global_xoff) {
  work_scheduler_.SetXoff(request_xoff, global_xoff);
}

void RdmaFalconModel::HandleCompletion(QpId qp_id, uint32_t rsn,
                                       Packet::Syndrome syndrome,
                                       uint8_t destination_bifurcation_id) {
  qp_manager_.InitiateQpLookup(
      qp_id, [this, qp_id, rsn, syndrome, destination_bifurcation_id](
                 absl::StatusOr<BaseQpContext*> qp_context) {
        CHECK_OK(qp_context.status())
            << "Received packet destined for unknown qp id: "
            << qp_context.status() << qp_id << "\n";
        pipeline_.emplace();
        pipeline_.back().is_completion = true;
        pipeline_.back().rsn = rsn;
        pipeline_.back().syndrome = syndrome;
        pipeline_.back().context =
            down_cast<FalconQpContext*>(qp_context.value());
        pipeline_.back().destination_bifurcation_id =
            destination_bifurcation_id;
        if (!pipeline_active_) {
          pipeline_active_ = true;
          PipelineDequeue();
        }
      });
}

void RdmaFalconModel::BackpressureQP(QpId qp_id, BackpressureType type,
                                     BackpressureData data) {
  BaseQpContext* ret = qp_manager_.DirectQpLookup(qp_id);
  auto context = down_cast<FalconQpContext*>(ret);
  switch (type) {
    case BackpressureType::kXoff: {
      if (data.xoff.request && !context->qp_request_xoff) {
        // Request xoff is being asserted by Falcon.
        context->last_request_xoff_assert_time = env_->ElapsedTime();
      } else if (!data.xoff.request && context->qp_request_xoff) {
        // We transitioned from request xoff asserted to de-asserted. Calculate
        // pause time and update metric.
        context->total_request_xoff_time_ns += absl::ToInt64Nanoseconds(
            env_->ElapsedTime() - context->last_request_xoff_assert_time);
        CollectScalarStats(absl::Substitute(kStatScalarQpRequestXoffTime, qp_id,
                                            context->scid),
                           context->total_request_xoff_time_ns);
      }

      if (data.xoff.response && !context->qp_response_xoff) {
        // Response xoff is being asserted by Falcon.
        context->last_response_xoff_assert_time = env_->ElapsedTime();
      } else if (!data.xoff.response && context->qp_response_xoff) {
        // We transitioned from response xoff asserted to de-asserted. Calculate
        // pause time and update metric.
        context->total_response_xoff_time_ns += absl::ToInt64Nanoseconds(
            env_->ElapsedTime() - context->last_response_xoff_assert_time);
        CollectScalarStats(absl::Substitute(kStatScalarQpResponseXoffTime,
                                            qp_id, context->scid),
                           context->total_response_xoff_time_ns);
      }

      context->qp_request_xoff = data.xoff.request;
      context->qp_response_xoff = data.xoff.response;

      if (!context->send_queue.empty() && !context->qp_request_xoff &&
          work_scheduler_.HasCredits(context, WorkType::kRequest)) {
        work_scheduler_.AddQpToScheduleSet(qp_id, WorkType::kRequest);
      }
      if (!context->inbound_read_request_queue.empty() &&
          !context->qp_response_xoff &&
          work_scheduler_.HasCredits(context, WorkType::kResponse)) {
        work_scheduler_.AddQpToScheduleSet(qp_id, WorkType::kResponse);
      }
      break;
    }
    case BackpressureType::kDynamicCredit: {
      // We update credit limits for all six types of Falcon resources.
      FalconCredit new_credit_limit = data.credit_limit;

      // We want to ensure the credit limit is greater than or equal to 1, and
      // smaller than the max credit count.
      new_credit_limit.request_tx_packet =
          std::clamp(new_credit_limit.request_tx_packet, (uint32_t)1,
                     config_.per_qp_credits().tx_packet_request());
      new_credit_limit.request_tx_buffer =
          std::clamp(new_credit_limit.request_tx_buffer, (uint32_t)1,
                     config_.per_qp_credits().tx_buffer_request());
      new_credit_limit.request_rx_packet =
          std::clamp(new_credit_limit.request_rx_packet, (uint32_t)1,
                     config_.per_qp_credits().rx_packet_request());
      new_credit_limit.request_rx_buffer =
          std::clamp(new_credit_limit.request_rx_buffer, (uint32_t)1,
                     config_.per_qp_credits().rx_buffer_request());
      new_credit_limit.response_tx_packet =
          std::clamp(new_credit_limit.response_tx_packet, (uint32_t)1,
                     config_.per_qp_credits().tx_packet_data());
      new_credit_limit.response_tx_buffer =
          std::clamp(new_credit_limit.response_tx_buffer, (uint32_t)1,
                     config_.per_qp_credits().tx_buffer_data());

      // If any new credit limit becomes larger than the previous credit limit,
      // we deem a credit increase.
      bool if_increase_credit = !(new_credit_limit <= context->credit_limit);

      // Update the per-QP credit limit.
      context->credit_limit = new_credit_limit;

      if (if_increase_credit) {
        // If these credits make the QP schedulable, notify the work scheduler.
        if (!context->send_queue.empty() && !context->qp_request_xoff &&
            work_scheduler_.HasCredits(context, WorkType::kRequest)) {
          work_scheduler_.AddQpToScheduleSet(qp_id, WorkType::kRequest);
        }
        if (!context->inbound_read_request_queue.empty() &&
            !context->qp_response_xoff &&
            work_scheduler_.HasCredits(context, WorkType::kResponse)) {
          work_scheduler_.AddQpToScheduleSet(qp_id, WorkType::kResponse);
        }
      }
      break;
    }
    default:
      LOG(FATAL) << "Unsupported backpressure policy provided.";
  }
}

void RdmaFalconModel::ProcessCompletion(FalconQpContext* context, uint32_t rsn,
                                        Packet::Syndrome syndrome,
                                        uint8_t destination_bifurcation_id) {
  auto it = context->rsn_to_op.find(rsn);
  CHECK(it != context->rsn_to_op.end());
  RdmaOp* rdma_op = it->second;
  rdma_op->n_seqs_finished++;
  // If n_seqs > 0 (i.e., the last request of this op has been sent), and
  // finished sequences == n_seqs, do completion.
  if (rdma_op->n_seqs > 0 && rdma_op->n_seqs_finished == rdma_op->n_seqs) {
    CompletionCallback completion_callback =
        std::move(rdma_op->completion_callback);
    CollectOpStats(context, rdma_op->end_seq);
    context->completion_queue.erase(rdma_op->end_seq);
    if (config_.enable_pcie_delay_for_completion()) {
      rdma_per_host_rx_buffers_->Push(
          destination_bifurcation_id, /*pcie_payload=*/0,
          /*pcie_issue_callback=*/nullptr,
          /*pcie_complete_callback=*/[completion_callback, syndrome]() {
            completion_callback(syndrome);
          });
    } else {
      completion_callback(syndrome);
    }
  }
  // Erase this mapping from RSN to op.
  context->rsn_to_op.erase(it);
}

bool RdmaFalconModel::IsRsnAcceptable(FalconQpContext* context,
                                      uint32_t rsn) const {
  if (context->rc_mode == RdmaConnectedMode::kOrderedRc &&
      rsn != context->next_rx_rsn) {
    if (!context->in_rnr)
      LOG(FATAL)
          << "Cannot accept an out-of-order RSN when QP is not in RNR state.";
    return false;
  }
  return true;
}

void RdmaFalconModel::UpdateNextRsnToReceive(FalconQpContext* context) {
  if (context->rc_mode == RdmaConnectedMode::kOrderedRc) {
    context->next_rx_rsn++;
  }
}

void RdmaFalconModel::EnterRnrState(FalconQpContext* context) {
  // Only ordered RC remembers RNR state.
  if (context->rc_mode == RdmaConnectedMode::kOrderedRc) {
    context->in_rnr = true;
  }
}

void RdmaFalconModel::LeaveRnrState(FalconQpContext* context) {
  // Only ordered RC remembers RNR state.
  if (context->rc_mode == RdmaConnectedMode::kOrderedRc) {
    context->in_rnr = false;
  }
}

bool RdmaFalconModel::ShouldRandomRnrNackWrite() const {
  std::uniform_real_distribution<> dist(0, 1.0);
  return (dist(*env_->GetPrng()) < write_random_rnr_probability_);
}

bool RdmaFalconModel::ShouldRandomRnrNackRead() const {
  std::uniform_real_distribution<> dist(0, 1.0);
  return (dist(*env_->GetPrng()) < read_random_rnr_probability_);
}

void RdmaFalconModel::CollectOpStats(FalconQpContext* context, uint32_t rsn) {
  auto it = context->completion_queue.find(rsn);
  RdmaOp& op = *(it->second);
  op.stats.completion_timestamp = env_->ElapsedTime();
  uint32_t op_id = ++context->total_ops_completed;
  uint32_t qp_id = context->qp_id;
  uint32_t cid = context->scid;
  double total_latency_ns = absl::ToDoubleNanoseconds(
      op.stats.completion_timestamp - op.stats.post_timestamp);
  double transport_latency_ns = absl::ToDoubleNanoseconds(
      op.stats.completion_timestamp - op.stats.start_timestamp);
  double queueing_latency_ns = absl::ToDoubleNanoseconds(
      op.stats.start_timestamp - op.stats.post_timestamp);

  // Update Tdigest histograms with Op latencies.
  if (stats_collection_flags_.enable_histograms()) {
    op_total_latency_->Add(total_latency_ns);
    op_transport_latency_->Add(transport_latency_ns);
    op_queueing_latency_->Add(queueing_latency_ns);
  }

  if (!stats_collection_flags_.enable_op_timeseries()) {
    return;
  }
  CollectScalarStats(absl::Substitute(kStatVectorOpLength, qp_id, cid, op_id),
                     op.length);
  CollectScalarStats(absl::Substitute(kStatVectorOpStartRsn, qp_id, cid, op_id),
                     op.start_seq);
  CollectScalarStats(absl::Substitute(kStatVectorOpEndRsn, qp_id, cid, op_id),
                     op.end_seq);
  CollectScalarStats(
      absl::Substitute(kStatVectorOpPostTimestamp, qp_id, cid, op_id),
      absl::ToDoubleNanoseconds(op.stats.post_timestamp));
  CollectScalarStats(
      absl::Substitute(kStatVectorOpStartTimestamp, qp_id, cid, op_id),
      absl::ToDoubleNanoseconds(op.stats.start_timestamp));
  CollectScalarStats(
      absl::Substitute(kStatVectorOpFinishTimestamp, qp_id, cid, op_id),
      absl::ToDoubleNanoseconds(op.stats.finish_timestamp));
  CollectScalarStats(
      absl::Substitute(kStatVectorOpCompletionTimestamp, qp_id, cid, op_id),
      absl::ToDoubleNanoseconds(op.stats.completion_timestamp));
  CollectScalarStats(
      absl::Substitute(kStatVectorOpTotalLatency, qp_id, cid, op_id),
      total_latency_ns);
  CollectScalarStats(
      absl::Substitute(kStatVectorOpTransportLatency, qp_id, cid, op_id),
      transport_latency_ns);
}

void RdmaFalconModel::DumpLatencyHistogramsToProto(
    RdmaLatencyHistograms* histograms) {
  op_total_latency_->ToProto(histograms->mutable_op_total_latency());
  op_transport_latency_->ToProto(histograms->mutable_op_transport_latency());
  op_queueing_latency_->ToProto(histograms->mutable_op_queued_latency());
}

}  // namespace isekai
