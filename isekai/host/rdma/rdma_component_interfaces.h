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

#ifndef ISEKAI_HOST_RDMA_RDMA_COMPONENT_INTERFACES_H_
#define ISEKAI_HOST_RDMA_RDMA_COMPONENT_INTERFACES_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/net_address.h"
#include "isekai/common/packet.h"

namespace isekai {

inline constexpr QpId kInvalidQpId = 0;

// Represents an RDMA operation, such as Read, Write, Send, Recv or Atomics.
struct RdmaOp {
  const RdmaOpcode opcode;
  // Exactly one of sgl or inline_payload_length must be set.
  std::vector<uint32_t> sgl;
  const uint32_t inline_payload_length = 0;
  // dest_qp_id is required in UD mode.
  QpId dest_qp_id = kInvalidQpId;
  // Completion callback function that is called when the operation finishes.
  const CompletionCallback completion_callback;

  // Each OP is divided into multiple lower layer units. In Falcon, each
  // OP is divided into multiple requests (or transactions). start_seq is
  // the request sequence number (rsn) of the first request on this OP, and
  // end_seq is the request sequence number (rsn) of the last request of this
  // OP.
  // BEGIN_GOOGLE_INTERNAL
  // With RoCE, seq represents packet sequence number (psn).
  // END_GOOGLE_INTERNAL
  uint32_t start_seq = 0, end_seq = 0;
  // Length of the op.
  uint32_t length = 0;
  // Number of request sequence numbers (rsn) of this op.
  // BEGIN_GOOGLE_INTERNAL
  // With RoCE, this is the number of packets.
  // END_GOOGLE_INTERNAL
  uint32_t n_seqs = 0;
  // Number of request sequence numbers (rsn) finished.
  // BEGIN_GOOGLE_INTERNAL
  // With RoCE, this is the number of packets finished.
  // END_GOOGLE_INTERNAL
  uint32_t n_seqs_finished = 0;

  // Collects per-op latency statistics.
  struct {
    bool is_scheduled = false;
    absl::Duration post_timestamp;        // RdmaOp was given to RDMA.
    absl::Duration start_timestamp;       // First packet was sent downstream.
    absl::Duration finish_timestamp;      // Last packet was sent downstream.
    absl::Duration completion_timestamp;  // Last completion/data was received.
  } stats;

  RdmaOp(RdmaOpcode opcode, std::vector<uint32_t> sgl,
         CompletionCallback completion_callback, QpId dest_qp_id)
      : opcode(opcode),
        sgl(std::move(sgl)),
        dest_qp_id(dest_qp_id),
        completion_callback(std::move(completion_callback)) {
    length = 0;
    for (auto sg_len : this->sgl) {
      length += sg_len;
    }
  }

  RdmaOp(RdmaOpcode opcode, uint32_t inline_payload_length,
         CompletionCallback completion_callback, QpId dest_qp_id)
      : opcode(opcode),
        inline_payload_length(inline_payload_length),
        dest_qp_id(dest_qp_id),
        completion_callback(std::move(completion_callback)),
        length(inline_payload_length) {}
};

// An incoming read request arriving from the network from a remote RDMA node.
struct InboundReadRequest {
  // Source rsn.
  const uint32_t rsn = 0;
  // Request scatter-gather list.
  const std::vector<uint32_t> sgl;

  uint32_t length = 0;

  InboundReadRequest(uint32_t rsn, std::vector<uint32_t> sgl)
      : rsn(rsn), sgl(std::move(sgl)) {
    length = 0;
    for (auto sg_len : sgl) length += sg_len;
  }
};

// QueuePair context state that is stored for each QP.
struct BaseQpContext {
  // Local RDMA QP id.
  QpId qp_id = kInvalidQpId;
  // Destination RDMA QP id, if QP is connected. Otherwise kInvalidQpId.
  QpId dest_qp_id = kInvalidQpId;
  // Connected QP ordering mode.
  RdmaConnectedMode rc_mode = RdmaConnectedMode::kUnorderedRc;
  // Destination IP address.
  Ipv6Address dst_ip;

  // Work queues for different types of RDMA operations.
  std::deque<std::unique_ptr<RdmaOp>> send_queue;
  std::deque<std::unique_ptr<RdmaOp>> receive_queue;
  std::deque<InboundReadRequest> inbound_read_request_queue;
  // Completion queue for requests sent out to the network (or Falcon). Contains
  // the original RDMA Op along with the completion callback that was provided
  // when issuing the op. It is keyed by the RSN (PSN) of the last request
  // (packet) associated with the RdmaOp. The RdmaOp is inserted into this map
  // once the last request (packet) is sent out, but the callback is executed
  // only when all the ACKs are received.
  absl::flat_hash_map<uint32_t, std::unique_ptr<RdmaOp>> completion_queue;
  // Map from rsn to the RdmaOp.
  absl::flat_hash_map<uint32_t, RdmaOp*> rsn_to_op;
  uint32_t total_ops_completed = 0;

  inline bool is_connected() { return dest_qp_id != kInvalidQpId; }
  virtual ~BaseQpContext() = default;
};

struct FalconQpContext : BaseQpContext {
  // Falcon source connection id.
  uint32_t scid;
  // The maximum credit usage of this QP.
  FalconCredit credit_limit;
  // Current credit usage for Falcon resource pools.
  FalconCredit credit_usage;
  // Next RSN for packets of this QP.
  uint32_t next_rsn = 0;
  // Tracks the QP's request/response xoff status by Falcon.
  bool qp_request_xoff = false;
  bool qp_response_xoff = false;
  // Number of outstanding outbound read requests.
  uint32_t outbound_read_requests = 0;
  // If this QP is in RNR state or not.
  bool in_rnr = false;
  // Next Rx RSN to process.
  uint32_t next_rx_rsn = 0;
  // RNR timeout.
  absl::Duration rnr_timeout = absl::ZeroDuration();
  // Next schedule time (required to enforce per-qo op rate limits).
  absl::Duration next_schedule_time = absl::ZeroDuration();
  // Variables to track the total time this QP was xoff'ed by Falcon.
  absl::Duration last_request_xoff_assert_time;
  absl::Duration last_response_xoff_assert_time;
  uint32_t total_request_xoff_time_ns;
  uint32_t total_response_xoff_time_ns;
};

class RdmaQpManagerInterface {
 public:
  virtual ~RdmaQpManagerInterface() {}
  // Creates a QueuePair with the given local qp id and QpOptions.
  virtual void CreateQp(std::unique_ptr<BaseQpContext> context) = 0;
  // Connects the given local QueuePair with remote QueuePair.
  virtual void ConnectQp(QpId local_qp_id, QpId remote_qp_id,
                         RdmaConnectedMode rc_mode) = 0;
  // Looks up a QueuePair using qp_id and executes the callback after accounting
  // for caching behavior and DRAM latency/bandwidth.
  virtual void InitiateQpLookup(
      QpId qp_id,
      absl::AnyInvocable<void(absl::StatusOr<BaseQpContext*>)> callback) = 0;
  // Performs a direct lookup using qp_id and return context immediately.
  virtual BaseQpContext* DirectQpLookup(QpId qp_id) = 0;
};

// Types of work a QP has to process. Request (as the initiator) corresponds to
// Send Queue and response (as the target) corresponds to Inbound Read Request
// Queue.
enum class WorkType {
  kRequest = 0,
  kResponse = 1,
};

// Identifies and input queue to the RDMA Work Scheduler.
struct WorkQueue {
  QpId qp_id;
  WorkType work_type;

  template <typename H>
  friend H AbslHashValue(H h, const WorkQueue& s) {
    return H::combine(std::move(h), s.qp_id, s.work_type);
  }

  inline bool operator==(const WorkQueue& s) const {
    return (qp_id == s.qp_id && work_type == s.work_type);
  }

  inline bool operator<(const WorkQueue& s) const {
    if (qp_id < s.qp_id) {
      return true;
    } else if (qp_id == s.qp_id) {
      if (work_type < s.work_type) {
        return true;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }

  WorkQueue(QpId qp_id, WorkType work_type)
      : qp_id(qp_id), work_type(work_type) {}
};

class RdmaWorkSchedulerInterface {
 public:
  virtual ~RdmaWorkSchedulerInterface() {}
  // Adds a WorkQueue to the set of active WQs being scheduled by the scheduler.
  // The WorkQueue is identified by a QpId and WorkType, and must have pending
  // ops and sufficient credits when added for scheduling.
  virtual void AddQpToScheduleSet(QpId qp_id, WorkType work_type) = 0;
  // Asserts Xoff so that the work scheduler stops sending any new requests
  // (request_xoff) or both request and responses (global_xoff) downstream.
  virtual void SetXoff(bool request_xoff, bool global_xoff) = 0;
  // Connects the work scheduler to a Falcon interface which it uses for sending
  // request and response transactions.
  virtual void ConnectFalconInterface(FalconInterface* falcon) = 0;
};

class RdmaFreeListManagerInterface {
 public:
  virtual ~RdmaFreeListManagerInterface() {}
  virtual absl::Status AllocateResource(QpId qp_id) = 0;
  virtual absl::Status FreeResource(QpId qp_id) = 0;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_RDMA_RDMA_COMPONENT_INTERFACES_H_
