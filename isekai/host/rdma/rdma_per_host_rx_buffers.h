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

#ifndef ISEKAI_HOST_RDMA_RDMA_PER_HOST_RX_BUFFERS_H_
#define ISEKAI_HOST_RDMA_RDMA_PER_HOST_RX_BUFFERS_H_

#include <cstdint>
#include <memory>
#include <queue>
#include <utility>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/host/rnic/memory_interface.h"

namespace isekai {

// RDMA buffer item definition consisting of the callbacks associated with
// PCIe issue/complete along with the payload size.
struct HostBufferItem {
  // Callback when PCIe transaction is issued by RDMA.
  absl::AnyInvocable<void()> pcie_issue_callback_;
  // Callback when the transaction is complete (payload transferred to host).
  absl::AnyInvocable<void()> pcie_complete_callback_;
  // Length of payload to be transferred over PCIe.
  uint64_t payload_length_;

  HostBufferItem(absl::AnyInvocable<void()> pcie_issue_callback,
                 absl::AnyInvocable<void()> pcie_complete_callback,
                 uint64_t payload_length)
      : pcie_issue_callback_(std::move(pcie_issue_callback)),
        pcie_complete_callback_(std::move(pcie_complete_callback)),
        payload_length_(std::move(payload_length)) {}
};

// Abstraction of per-host buffer queue.
struct HostBuffer {
  std::queue<HostBufferItem> buffer_;
  uint32_t max_size_bytes_;
  uint32_t current_size_bytes_;

  explicit HostBuffer(uint32_t max_size_bytes)
      : max_size_bytes_(std::move(max_size_bytes)), current_size_bytes_(0) {}
  HostBuffer() = delete;
  HostBuffer(const HostBuffer&) = delete;
  HostBuffer& operator=(const HostBuffer&) = delete;
};

// Abstraction for RDMA host buffers. It stores incoming RDMA buffer items into
// the corresponding queues. Once the host queue gets full, this abstraction
// xoffs the Falcon inter-host Rx scheduler for the appropriate host.
class RdmaPerHostRxBuffers {
 public:
  // Constructor.
  RdmaPerHostRxBuffers(const RdmaRxBufferConfig& config,
                       std::vector<std::unique_ptr<MemoryInterface>>* hif,
                       FalconInterface* falcon);

  // Delete copy constructor and assignment operator.
  RdmaPerHostRxBuffers(const RdmaPerHostRxBuffers&) = delete;
  RdmaPerHostRxBuffers& operator=(const RdmaPerHostRxBuffers&) = delete;

  void Push(uint8_t host_id, uint32_t payload_length,
            absl::AnyInvocable<void()> pcie_issue_callback,
            absl::AnyInvocable<void()> pcie_complete_callback);

 private:
  // Pointer to host interface; there is one-one mapping from Falcon inter-host
  // RX schduler to RDMA buffer and host interface.
  std::vector<std::unique_ptr<MemoryInterface>>* const host_interface_;
  std::vector<std::unique_ptr<HostBuffer>> rx_buffer_;
  // Pointer to Falcon interface; used for xoff-ing the inter-host RX scheduler
  // for a given host.
  FalconInterface* const falcon_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_RDMA_RDMA_PER_HOST_RX_BUFFERS_H_
