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

#ifndef ISEKAI_HOST_FALCON_FALCON_RATE_UPDATE_ENGINE_ADAPTER_H_
#define ISEKAI_HOST_FALCON_FALCON_RATE_UPDATE_ENGINE_ADAPTER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <string_view>
#include <utility>

#include "absl/status/statusor.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/event_response_format_adapter.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/falcon/rue/format.h"

namespace isekai {

// Carries the required response metadata on dequeing a response from the this
// adapter that the rate update engine needs.
struct ResponseMetadata {
  bool randomize_path;
  bool has_rto_decreased;
  std::unique_ptr<RueKey> rue_key;
};

// Adapts the Falcon interaction to a custom RUE implementation.
class RueAdapterInterface {
 public:
  RueAdapterInterface() = default;
  virtual ~RueAdapterInterface() = default;

  // Disallow copy/assign
  RueAdapterInterface(const RueAdapterInterface&) = delete;
  RueAdapterInterface& operator=(const RueAdapterInterface&) = delete;

  // Processes one RUE Event.
  virtual void ProcessNextEvent(uint32_t now) = 0;

  // Returns the number of queued event in the RUE mailbox.
  virtual int GetNumEvents() const = 0;

  virtual void EnqueueAck(const Packet* packet,
                          const CongestionControlMetadata& ccmeta,
                          const RueKey* rue_key, uint32_t num_packets_acked,
                          bool eack, bool eack_drop) = 0;
  virtual void EnqueueNack(const RueKey* rue_key, const Packet* packet,
                           const CongestionControlMetadata& ccmeta,
                           uint32_t num_packets_acked) = 0;
  virtual void EnqueueTimeoutRetransmit(const RueKey* rue_key,
                                        const Packet* packet,
                                        const CongestionControlMetadata& ccmeta,
                                        uint8_t retransmit_count) = 0;

  virtual ResponseMetadata DequeueResponse(
      std::function<ConnectionState*(uint32_t connection_id)>
          connection_state_lookup) = 0;
  virtual void InitializeMetadata(
      CongestionControlMetadata& metadata) const = 0;
};

// The RUE Adapter is templatized on (1) the CC algorithm, (2) Event format, and
// (3) Response format. The CC algorithm should implement a `Process` function
// with the following signature:
//   void Process(const EventT& event, ResponseT& response, uint32_t now)
// The RUE Adapter class is responsible for (1) queueing and sending Events to
// the CC algorithm, and (2) accepting responses back from the CC algorithm and
// relaying them back to the RUE class.
template <typename AlgorithmT, typename EventT, typename ResponseT>
class RueAdapter : public RueAdapterInterface {
 public:
  explicit RueAdapter(
      std::unique_ptr<AlgorithmT> algorithm,
      std::unique_ptr<EventResponseFormatAdapter<EventT, ResponseT>>
          format_adapter)
      : algorithm_(std::move(algorithm)),
        format_adapter_(std::move(format_adapter)) {}
  ~RueAdapter() override = default;
  void ProcessNextEvent(uint32_t now) override;
  void EnqueueAck(const Packet* packet, const CongestionControlMetadata& ccmeta,
                  const RueKey* rue_key, uint32_t num_packets_acked, bool eack,
                  bool eack_drop) override;
  void EnqueueNack(const RueKey* rue_key, const Packet* packet,
                   const CongestionControlMetadata& ccmeta,
                   uint32_t num_packets_acked) override;
  void EnqueueTimeoutRetransmit(const RueKey* rue_key, const Packet* packet,
                                const CongestionControlMetadata& ccmeta,
                                uint8_t retransmit_count) override;
  ResponseMetadata DequeueResponse(
      std::function<ConnectionState*(uint32_t connection_id)>
          connection_state_lookup) override;
  int GetNumEvents() const override { return event_queue_.size(); }
  void InitializeMetadata(CongestionControlMetadata& metadata) const override {}

 private:
  std::unique_ptr<RueKey> GetRueKey(const ResponseT* response);
  std::queue<std::unique_ptr<EventT>> event_queue_;
  std::queue<std::unique_ptr<ResponseT>> response_queue_;
  std::unique_ptr<AlgorithmT> algorithm_;
  std::unique_ptr<EventResponseFormatAdapter<EventT, ResponseT>>
      format_adapter_;
};

template <typename AlgorithmT, typename EventT, typename ResponseT>
void RueAdapter<AlgorithmT, EventT, ResponseT>::ProcessNextEvent(uint32_t now) {
  // Pulls out the next event and sets a scheduled event to process the queue
  auto event = std::move(event_queue_.front());
  event_queue_.pop();

  // Uses the algorithm implementation to get the RUE response
  auto response_ptr = std::make_unique<ResponseT>();
  response_queue_.push(std::move(response_ptr));

  algorithm_->Process(*event, *(response_queue_.back()), now);
}

template <typename AlgorithmT, typename EventT, typename ResponseT>
void RueAdapter<AlgorithmT, EventT, ResponseT>::EnqueueAck(
    const Packet* packet, const CongestionControlMetadata& ccmeta,
    const RueKey* rue_key, uint32_t num_packets_acked, bool eack,
    bool eack_drop) {
  EventT event = {};
  format_adapter_->FillExplicitAckEvent(event, rue_key, packet, ccmeta,
                                        num_packets_acked, eack, eack_drop);
  event_queue_.push(std::make_unique<EventT>(event));
}

template <typename AlgorithmT, typename EventT, typename ResponseT>
void RueAdapter<AlgorithmT, EventT, ResponseT>::EnqueueNack(
    const RueKey* rue_key, const Packet* packet,
    const CongestionControlMetadata& ccmeta, uint32_t num_packets_acked) {
  EventT event = {};
  format_adapter_->FillNackEvent(event, rue_key, packet, ccmeta,
                                 num_packets_acked);
  event_queue_.push(std::make_unique<EventT>(event));
}

template <typename AlgorithmT, typename EventT, typename ResponseT>
void RueAdapter<AlgorithmT, EventT, ResponseT>::EnqueueTimeoutRetransmit(
    const RueKey* rue_key, const Packet* packet,
    const CongestionControlMetadata& ccmeta, uint8_t retransmit_count) {
  EventT event = {};
  format_adapter_->FillTimeoutRetransmittedEvent(event, rue_key, packet, ccmeta,
                                                 retransmit_count);
  event_queue_.push(std::make_unique<EventT>(event));
}

template <typename AlgorithmT, typename EventT, typename ResponseT>
ResponseMetadata RueAdapter<AlgorithmT, EventT, ResponseT>::DequeueResponse(
    std::function<ConnectionState*(uint32_t connection_id)>
        connection_state_lookup) {
  // Dequeue the response from the queue.
  const auto response = std::move(response_queue_.front());
  response_queue_.pop();
  // Update the connection state with the response.
  auto cid = response->connection_id;
  auto connection_state = connection_state_lookup(cid);
  absl::Duration original_retransmit_timeout =
      connection_state->congestion_control_metadata->retransmit_timeout;
  format_adapter_->UpdateConnectionStateFromResponse(connection_state,
                                                     response.get());
  return ResponseMetadata{
      .randomize_path = format_adapter_->IsRandomizePath(response.get()),
      .has_rto_decreased =
          connection_state->congestion_control_metadata->retransmit_timeout <
          original_retransmit_timeout,
      .rue_key = GetRueKey(response.get())};
}

template <typename AlgorithmT, typename EventT, typename ResponseT>
std::unique_ptr<RueKey> RueAdapter<AlgorithmT, EventT, ResponseT>::GetRueKey(
    const ResponseT* response) {
  if constexpr (std::is_same_v<ResponseT, falcon_rue::Response>) {
    return std::make_unique<RueKey>(response->connection_id);
  } else {
    return std::make_unique<Gen2RueKey>(response->connection_id,
                                        response->flow_id);
  }
}

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_RATE_UPDATE_ENGINE_ADAPTER_H_
