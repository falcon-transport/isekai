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

#include "isekai/host/rnic/memory_interface.h"

#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"

namespace isekai {

// Constructor for MemoryInterface which consists of write and read queues.
MemoryInterface::MemoryInterface(const MemoryInterfaceConfig& config,
                                 Environment* env)
    : write_queue_(MemoryInterfaceQueue(config.write_queue_config(), env)),
      read_queue_(MemoryInterfaceQueue(config.read_queue_config(), env)) {}

// Constructor for write or read memory interface queue.
MemoryInterfaceQueue::MemoryInterfaceQueue(
    const MemoryInterfaceConfig::MemoryQueueInterfaceConfig& config,
    Environment* env)
    : config_(config), env_(env) {
  CHECK(env_) << "The input environment is nullptr";
  max_queue_size_packets_ = config.memory_interface_queue_size_packets();
  switch (config_.delay_distribution()) {
    case MemoryInterfaceConfig_MemoryDelayDistribution_CONST:
      break;
    case MemoryInterfaceConfig_MemoryDelayDistribution_UNIFORM: {
      pcie_delay_distribution_ = std::uniform_int_distribution<uint64_t>(
          config_.memory_delay_uniform_low_ns(),
          config_.memory_delay_uniform_high_ns());
      break;
    }
    case MemoryInterfaceConfig_MemoryDelayDistribution_EXPONENTIAL: {
      double lambda = 1.0 / config_.memory_delay_exponential_mean_ns();
      pcie_delay_distribution_ = std::exponential_distribution<>(lambda);
      break;
    }
    case MemoryInterfaceConfig_MemoryDelayDistribution_DISCRETE: {
      ParseDiscreteRandomDistribution();
      break;
    }
  }
}

// Setter for wakeup callback. Sender will set up the wakeup callback to get the
// notification about the availability in the MemoryInterface queue.
void MemoryInterfaceQueue::set_wakeup_callback(
    absl::AnyInvocable<void()> wakeup_callback) {
  wakeup_callback_ = std::move(wakeup_callback);
}

//
// mapping.
// Parser for the DISCRETE delay.
void MemoryInterfaceQueue::ParseDiscreteRandomDistribution() {
  std::vector<int> discrete_delay_prob;
  auto s = config_.memory_delay_discrete_mapping() + ",";
  int start = 0, end = s.find(',');
  while (end != -1) {
    // Get each delay:prob pair.
    auto delay_prob = s.substr(start, end - start);
    int pos = delay_prob.find(':');
    auto delay = std::stoi(delay_prob.substr(0, pos));
    auto prob =
        std::stoi(delay_prob.substr(pos + 1, delay_prob.size() - pos - 1));
    discrete_delay_ns_.push_back(delay);
    discrete_delay_prob.push_back(prob);
    start = end + 1;
    end = s.find(',', start);
  }
  pcie_delay_distribution_ = std::discrete_distribution<>(
      discrete_delay_prob.begin(), discrete_delay_prob.end());
}

// This function schedules (transmits the given tx item) based on the configured
// bandwidth (transmission time). If the bandwidth is set to 0, assume it is
// infinite bandwidth. It also schedules callback function after transmission
// time and configured memory link delay.
void MemoryInterfaceQueue::TransmitStart(MemoryTxMetadata tx) {
  CHECK(tx_machine_state_ == READY) << "Must be READY to transmit";
  tx_machine_state_ = BUSY;

  absl::Duration tx_time = absl::ZeroDuration();
  if (config_.bandwidth_bps() != 0) {
    tx_time =
        absl::Seconds(tx.size_bytes_ * 8.0 / (1.0 * config_.bandwidth_bps()));
  }

  CHECK_OK(env_->ScheduleEvent(tx_time, [this]() { TransmitComplete(); }));
  if (tx.callback_) {
    CHECK_OK(env_->ScheduleEvent(tx_time + GetRandomPCIeDelay(),
                                 std::move(tx.callback_)));
  }
}

// This function is invoked upon completion of transmission. It checks if there
// are any outstanding packets. If yes, it pops the item from the tx queue and
// starts sending it. It also invokes wakeup callback to notify the user
// about the space availability in the transmission queue.
void MemoryInterfaceQueue::TransmitComplete() {
  CHECK(tx_machine_state_ == BUSY) << "Must be BUSY if transmitting";
  tx_machine_state_ = READY;

  if (tx_queue_.empty()) {
    if (wakeup_callback_) {
      wakeup_callback_();
    }
    return;
  }
  bool was_queue_full = tx_queue_.size() == max_queue_size_packets_;
  TransmitStart(std::move(tx_queue_.front()));
  tx_queue_.pop();
  if (wakeup_callback_ && was_queue_full) {
    // Notify the user of this interface to let them know about the space
    // availability in this interface queue. Now, the interface user can send
    // the data packets at certain rate till the queue gets full.
    wakeup_callback_();
  }
}

// Checks if memory interface can accept a packet or not.
absl::Status MemoryInterfaceQueue::CanEnqueuePacket() {
  if (tx_queue_.size() == max_queue_size_packets_) {
    return absl::ResourceExhaustedError("Tx queue is full");
  }
  return absl::OkStatus();
}

// Initiates the transaction that models writing/reading to memory.
void MemoryInterfaceQueue::EnqueuePacket(uint64_t size_bytes,
                                         absl::AnyInvocable<void()> callback) {
  tx_queue_.emplace(std::move(callback), size_bytes);
  if (tx_machine_state_ == READY) {
    TransmitStart(std::move(tx_queue_.front()));
    tx_queue_.pop();
  }
}

// Sets up the write callback which is called once there is space available in
// the write queue.
void MemoryInterface::SetWriteCallback(
    absl::AnyInvocable<void()> wakeup_callback) {
  write_queue_.set_wakeup_callback(std::move(wakeup_callback));
}

// Sets up the read callback which is called once there is space available in
// the read queue.
void MemoryInterface::SetReadCallback(
    absl::AnyInvocable<void()> wakeup_callback) {
  read_queue_.set_wakeup_callback(std::move(wakeup_callback));
}

// Checks if memory interface can accept a read transaction.
absl::Status MemoryInterface::CanReadFromMemory() {
  return read_queue_.CanEnqueuePacket();
}

// Models the behavior of reading from the host. Currently not supported.
void MemoryInterface::ReadFromMemory(uint64_t size_bytes,
                                     absl::AnyInvocable<void()> callback) {
  read_queue_.EnqueuePacket(size_bytes, std::move(callback));
}

// Checks if memory interface can accept a write transaction.
absl::Status MemoryInterface::CanWriteToMemory() {
  return write_queue_.CanEnqueuePacket();
}

// Initiates the transaction that models writing to memory.
void MemoryInterface::WriteToMemory(uint64_t size_bytes,
                                    absl::AnyInvocable<void()> callback) {
  write_queue_.EnqueuePacket(size_bytes, std::move(callback));
}

// Returns link delay based on the configuration.
absl::Duration MemoryInterfaceQueue::GetRandomPCIeDelay() {
  switch (config_.delay_distribution()) {
    case MemoryInterfaceConfig_MemoryDelayDistribution_CONST:
      return absl::Nanoseconds(config_.memory_delay_const_ns());
    case MemoryInterfaceConfig_MemoryDelayDistribution_UNIFORM:
      return absl::Nanoseconds(
          std::get<std::uniform_int_distribution<uint64_t>>(
              pcie_delay_distribution_)(*env_->GetPrng()));
    case MemoryInterfaceConfig_MemoryDelayDistribution_EXPONENTIAL:
      return absl::Nanoseconds(
          (uint64_t)std::get<std::exponential_distribution<>>(
              pcie_delay_distribution_)(*env_->GetPrng()));
    case MemoryInterfaceConfig_MemoryDelayDistribution_DISCRETE:
      return absl::Nanoseconds(
          discrete_delay_ns_[std::get<std::discrete_distribution<>>(
              pcie_delay_distribution_)(*env_->GetPrng())]);
  }
}

}  // namespace isekai
