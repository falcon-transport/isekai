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

#ifndef ISEKAI_HOST_FALCON_FALCON_PROTOCOL_CONNECTION_SCHEDULER_TYPES_H_
#define ISEKAI_HOST_FALCON_FALCON_PROTOCOL_CONNECTION_SCHEDULER_TYPES_H_

#include <cstdint>
#include <queue>

#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "isekai/host/falcon/falcon.h"

namespace isekai {

// Distance-based comparators to handle PSN wraparounds.
inline bool SeqLT(uint32_t a, uint32_t b) {
  return ((static_cast<int32_t>(a - b)) < 0);
}
inline bool SeqLEQ(uint32_t a, uint32_t b) {
  return ((static_cast<int32_t>(a - b)) <= 0);
}
inline bool SeqGT(uint32_t a, uint32_t b) {
  return ((static_cast<int32_t>(a - b)) > 0);
}
inline bool SeqGEQ(uint32_t a, uint32_t b) {
  return ((static_cast<int32_t>(a - b)) >= 0);
}

// A work item submitted to the connection scheduler is uniquely identified by
// the (rsn, transaction type) pair.
struct WorkId {
  uint32_t rsn;
  falcon::PacketType type;
  WorkId() {}
  explicit WorkId(uint32_t rsn, falcon::PacketType type)
      : rsn(rsn), type(type) {}
};

// A work item submitted to the retransmission scheduler is uniquely identified
// by the (rsn, psn, transaction type) tuple.
struct RetransmissionWorkId {
  uint32_t rsn;  // Holds RSN of the packet that needs to be retransmitted.
  uint32_t psn;  // Holds PSN of the packet that needs to be retransmitted.
  falcon::PacketType type;
  RetransmissionWorkId() {}
  explicit RetransmissionWorkId(uint32_t rsn, uint32_t psn,
                                falcon::PacketType type)
      : rsn(rsn), psn(psn), type(type) {}
  // Work IDs with lower PSN are prioritized as retransmissions need to happen
  // in PSN order.
  bool operator<(const RetransmissionWorkId& rwd) const {
    return SeqLT(psn, rwd.psn);
  }
};

// Represents the connection scheduler queue based on the packet types. Used by
// the connection scheduler and its policies for first-time transmissions.
enum class PacketTypeQueue {
  kPullAndOrderedPushRequest = 0,
  kUnorderedPushRequest = 1,
  kPushData = 2,
  kPushGrant = 3,
  kPullData = 4,
};

// Corresponds to the per connection scheduler queues used for first-time
// transmissions (by the connection scheduler).
class ConnectionSchedulerQueues {
 public:
  ConnectionSchedulerQueues();
  // Enqueues a work item into the specified queue type.
  void Enqueue(PacketTypeQueue queue_type, WorkId& work);
  // Returns a reference to the work item at the front of the specified queue.
  const WorkId& Peek(PacketTypeQueue queue_type);
  // Removes the work item at the front of the specified queue.
  void Pop(PacketTypeQueue queue_type);
  // Returns whether the specified queue type is empty.
  bool IsEmpty(PacketTypeQueue queue_type);

  // Return the queue length of an intra-connection queue.
  uint32_t GetSize(PacketTypeQueue queue_type);

 private:
  absl::flat_hash_map<PacketTypeQueue, std::queue<WorkId>> queues_;
};

// Represents the two kinds of windows - one for request window and another for
// data window.
enum class WindowTypeQueue {
  kRequest,
  kData,
};

// Corresponds to the per connection scheduler queues used for retransmissions
// by the retransmission scheduler.
class RetransmissionSchedulerQueues {
 public:
  RetransmissionSchedulerQueues();
  // Enqueues a retransmission work item into the specified queue type and
  // returns a bool to indicate if the insertion took place.
  bool Enqueue(WindowTypeQueue queue_type, RetransmissionWorkId& work);
  // Returns a reference to the work item at the front of the specified queue.
  const RetransmissionWorkId& Peek(WindowTypeQueue queue_type);
  // Removes the specified work item from the specified queue.
  bool Dequeue(WindowTypeQueue queue_type, RetransmissionWorkId& work_id);
  // Returns whether the specified queue type is empty.
  bool IsEmpty(WindowTypeQueue queue_type);

 private:
  absl::flat_hash_map<WindowTypeQueue, absl::btree_set<RetransmissionWorkId>>
      queues_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_PROTOCOL_CONNECTION_SCHEDULER_TYPES_H_
