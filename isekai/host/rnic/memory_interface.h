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

#ifndef ISEKAI_HOST_RNIC_MEMORY_INTERFACE_H_
#define ISEKAI_HOST_RNIC_MEMORY_INTERFACE_H_

#include <cstdint>
#include <queue>
#include <random>
#include <utility>
#include <variant>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"

namespace isekai {

class Environment;

// This structure defines the metadata for memory transaction. It consists of
// following:
// 1) size_bytes_ is the size of transaction (length of read/write data) which
// will be used to calculate trasmission time of the memory transaction. 2)
// callback_ is the function handler that will be called once the memory
// transaction completes (ie. after tramission time + latency delay).
struct MemoryTxMetadata {
  absl::AnyInvocable<void()> callback_;
  uint64_t size_bytes_;

  MemoryTxMetadata(absl::AnyInvocable<void()> callback, uint64_t size_bytes)
      : callback_(std::move(callback)), size_bytes_(std::move(size_bytes)) {}
};

class MemoryInterfaceQueue {
 public:
  explicit MemoryInterfaceQueue(
      const MemoryInterfaceConfig::MemoryQueueInterfaceConfig& config,
      Environment* env);

  // Sets the wakeup callback for the sender that is using this queue to let
  // them know that there is a space available for the sender to send more
  // transactions if needed.
  void set_wakeup_callback(absl::AnyInvocable<void()> wakeup_callback);
  absl::Status CanEnqueuePacket();
  void EnqueuePacket(uint64_t size_bytes, absl::AnyInvocable<void()> callback);

 private:
  // Starts the execution of the transaction (either read or write).
  void TransmitStart(MemoryTxMetadata tx_item);
  // Function called upon transaction execution completion to notify the user
  // to send more packets to this interface (via the callback).
  void TransmitComplete();

  // Function to get random PCIe delay.
  absl::Duration GetRandomPCIeDelay();
  // Parses Discrete random delay string.
  void ParseDiscreteRandomDistribution();

  const MemoryInterfaceConfig::MemoryQueueInterfaceConfig
      config_;              // Memory interface config (e.g.,
                            // delay, bandwidth).
  Environment* const env_;  // env to schedule the transmit event.

  // Variables for storing random delay.
  std::variant<std::uniform_int_distribution<uint64_t>,
               std::exponential_distribution<>, std::discrete_distribution<>>
      pcie_delay_distribution_;
  std::vector<uint64_t> discrete_delay_ns_;

  // Variable to maintain transmit state machine. When in READY state, call
  // TransmitStart function right away, otherwise just enqueue the transaction
  // in the egress queue.
  enum TxMachineState { READY, BUSY };
  TxMachineState tx_machine_state_{READY};
  //
  // simulating interface queue from host to RNIC. The current queue is only
  // responsible for handling Rx path.
  std::queue<MemoryTxMetadata> tx_queue_;
  uint64_t max_queue_size_packets_;
  absl::AnyInvocable<void()> wakeup_callback_;
};

// This class implements the model for a memory interface that has its own
// datarate and delay. The current implementation only models the write transmit
// queue.
// In the context of host memory interface, this interface helps: (1) the
// application to post ops (work queue entries) to the RDMA model, (2) the
// packet builder to read host memory for constructing packets, and (3) the RDMA
// block for writing incoming write ops to host memory. (4) the application to
// read entries from the completion queue (CQ).

// Example: Instead of the packet builder directly scheduling a packet enqueue
// to egress CoS queues (after simulating a PCIe read delay) as follows:
//   Environment.Schedule(delay, [..]() {
//     EgressQueue.push_back(packet);
//   }

// it calls,
//   MemoryInterface.ReadFromHost(size, [..]() {
//     EgressQueue.push_back(packet);
//   });

// This way the MemoryInterface can more accurately calculate the delay based on
// other concurrent ongoing memory operations and size of the transaction.
class MemoryInterface {
 public:
  MemoryInterface(const MemoryInterfaceConfig& config, Environment* env);

  // Delete copy constructor and assignment operator to enforce the use of copy
  // by reference instead of creating a local copy that can lead to
  // inconsistent/incorrect behavior.
  MemoryInterface(const MemoryInterface&) = delete;
  MemoryInterface& operator=(const MemoryInterface&) = delete;

  // Sets the wakeup callback for the sender that is using the write interface
  // to let them know that there is space available for the sender to send more
  // transactions if needed.
  void SetWriteCallback(absl::AnyInvocable<void()> wakeup_callback);

  // Sets the wakeup callback for the sender that is using the read interface
  // to let them know that there is space available for the sender to send more
  // transactions if needed.
  void SetReadCallback(absl::AnyInvocable<void()> wakeup_callback);

  // Returns the status of whether a read transaction can be accepted at the
  // memory interface. CanReadFromMemory() must return successfully before
  // calling ReadFromMemory(). Returns an absl::ResourceExhaustedError status if
  // the Tx queue is full.
  absl::Status CanReadFromMemory();
  void ReadFromMemory(uint64_t size_bytes, absl::AnyInvocable<void()> callback);
  // Returns the status of whether a write transaction can be accepted at the
  // memory interface. CanWriteToMemory() must return succesfully before calling
  // WriteToMemory(). Returns an absl::ResourceExhaustedError status if the Tx
  // queue is full.
  absl::Status CanWriteToMemory();
  void WriteToMemory(uint64_t size_bytes, absl::AnyInvocable<void()> callback);

 private:
  MemoryInterfaceQueue write_queue_;
  MemoryInterfaceQueue read_queue_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_RNIC_HOST_INTERFACE_H_
