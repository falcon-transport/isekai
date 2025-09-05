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

#ifndef ISEKAI_HOST_FALCON_FALCON_PROTOCOL_RATE_UPDATE_ENGINE_H_
#define ISEKAI_HOST_FALCON_FALCON_PROTOCOL_RATE_UPDATE_ENGINE_H_

#include <cstdint>
#include <memory>

#include "absl/container/flat_hash_map.h"
#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/rate_update_engine_adapter.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/fixed.h"
#include "isekai/host/falcon/rue/latency_generator.h"

namespace isekai {

// Forward declaration of ProtocolRateUpdateEngineTestPeer, declared as friend
// class of ProtocolRateUpdateEngine.
template <typename EventT, typename ResponseT>
class ProtocolRateUpdateEngineTestPeer;

class ProtocolRateUpdateEngine : public RateUpdateEngine {
 public:
  // These are the values this implementation uses to initialize metadata.
  //
  static inline const uint32_t kDefaultFabricCongestionWindow =
      falcon_rue::FloatToFixed<double, uint32_t>(32.0,
                                                 falcon_rue::kFractionalBits);
  static constexpr absl::Duration kDefaultInterPacketGap = absl::ZeroDuration();
  static constexpr uint32_t kDefaultNicCongestionWindow = 64;
  // Default retransmission timeout. The actual enforced retransmission timeout
  // is approximately equal to this as it is converted from FALCON unit time.
  static constexpr absl::Duration kDefaultRetransmitTimeout =
      absl::Nanoseconds(1000000);  // 1ms
  //
  static constexpr absl::Duration kDefaultBaseDelay = absl::Microseconds(25);
  static constexpr absl::Duration kDefaultDelayState = absl::Microseconds(25);
  static constexpr absl::Duration kDefaultRttState = absl::Microseconds(25);

  explicit ProtocolRateUpdateEngine(FalconModelInterface* falcon);
  ~ProtocolRateUpdateEngine() override = default;

  void InitializeMetadata(CongestionControlMetadata& metadata) override;
  void ExplicitAckReceived(const Packet* packet, bool eack,
                           bool eack_drop) override;
  void NackReceived(const Packet* packet) override;
  void PacketTimeoutRetransmitted(uint32_t cid, const Packet* packet,
                                  uint8_t retransmit_count) override;
  void PacketEarlyRetransmitted(uint32_t cid, const Packet* packet,
                                uint8_t retransmit_count) override;

  uint32_t ToFalconTimeUnits(absl::Duration time) const override;
  absl::Duration FromFalconTimeUnits(uint32_t time) const override;

  uint32_t ToTimingWheelTimeUnits(absl::Duration time) const override;
  absl::Duration FromTimingWheelTimeUnits(uint32_t time) const override;

  uint32_t GenerateRandomFlowLabel() const override;

 protected:
  void HandlePacketTimeoutRetransmitted(const RueKey* rue_key,
                                        const Packet* packet,
                                        uint8_t retransmit_count);
  std::unique_ptr<RueAdapterInterface> rue_adapter_;

  // Initializes the CongestionControlMetadata fields that are Gen-specific.
  // This causes Gen1-specific fields to not be initialized in Gen2 for example.
  virtual void InitializeGenSpecificMetadata(
      CongestionControlMetadata& metadata);

  // Handles logging stats (e.g., network delays) from the ACK packet received.
  virtual void CollectAckStats(const RueKey* rue_key, const Packet* packet);
  // Handles logging the num_acked stat for a RUE event by an ACK/NACK before
  // that event is enqueued to be sent to Swift.
  virtual void CollectNumAckedStats(const RueKey* rue_key,
                                    uint32_t num_packets_acked);
  // Handles logging the CC metrics that are output by a RUE response and
  // applied to the datapath (e.g., fcwnd, ncwnd, rto, flow repath).
  virtual void CollectCongestionControlMetricsAfterResponse(
      const RueKey* rue_key, const CongestionControlMetadata& metadata,
      const ResponseMetadata& response_metadata);

  FalconModelInterface* const falcon_;

 private:
  // Initializes the delay_state in CongestionControlMetadata.
  virtual void InitializeDelayState(CongestionControlMetadata& metadata) const;
  // Creates a RueKey struct from an incoming packet.
  std::unique_ptr<RueKey> GetRueKeyFromIncomingPacket(
      const Packet* packet) const;
  // Returns the last RUE event time for the input RueKey.
  virtual absl::Duration GetLastEventTime(const RueKey* rue_key) const;
  // Updates last RUE event time for the input RueKey to the current simulation
  // time .
  virtual void UpdateLastEventTime(const RueKey* rue_key) const;
  // Returns whether there is an outstanding event for the input RueKey.
  virtual bool GetOutstandingEvent(const RueKey* rue_key) const;
  // Updates the outstanding event state for the input RueKey to the input
  // value. Returns true if the previous value was different than the new input
  // value, false otherwise.
  virtual bool UpdateOutstandingEvent(const RueKey* rue_key, bool value) const;
  // Returns the num_acked value since the last successful RUE event for the
  // input RueKey.
  virtual uint32_t GetNumAcked(const RueKey* rue_key) const;
  // Resets the num_acked value for the input RueKey to 0.
  virtual void ResetNumAcked(const RueKey* rue_key) const;

  void TriggerEventProcessing(const RueKey* rue_key);
  void ProcessNextEvent();
  void HandleNextResponse();

  // Returns whether an event for the input RUE key can be enqueued to RUE.
  bool CanEnqueueEvent(const RueKey* rue_key) const;

  bool queue_scheduled_;

  double nanoseconds_per_falcon_time_unit_;
  uint64_t nanoseconds_per_timing_wheel_unit_;

  // In Gen2, initial_fcwnd_ specifies the connection-level initial fcwnd.
  uint32_t initial_fcwnd_ = kDefaultFabricCongestionWindow;
  uint32_t initial_ncwnd_ = kDefaultNicCongestionWindow;
  falcon::DelaySelect delay_select_ = falcon::DelaySelect::kFabric;
  absl::Duration base_delay_ = kDefaultBaseDelay;
  absl::Duration retransmission_timeout_;
  // Default time FALCON spends in creating the event and reading the response..
  absl::Duration default_falcon_latency_ns_;
  // Maximum number of events in the event queue.
  uint64_t event_queue_size_;
  // The 3 thresholds of event queue size for rate limiter.
  uint64_t event_queue_threshold_1_;
  uint64_t event_queue_threshold_2_;
  uint64_t event_queue_threshold_3_;
  // The time threshold of predicate 1.
  absl::Duration predicate_1_time_threshold_;
  // The packet threshold of predicate 2.
  uint32_t predicate_2_packet_count_threshold_;
  // RUE processing latency generator.
  std::unique_ptr<LatencyGeneratorInterface> rue_processing_latency_gen_;
  // Records the PLB reroute count for each cid.
  absl::flat_hash_map</*cid=*/uint32_t, uint32_t> plb_reroute_count;

  template <typename EventT, typename ResponseT>
  friend class ProtocolRateUpdateEngineTestPeer;
  friend class ProtocolRateUpdateAdapterTestPeer;
};

// Define `Gen1RateUpdateEngine` to be the instantiation of
// the `ProtocolRateUpdateEngine` class with the Gen1 Event and Response
// formats.
typedef ProtocolRateUpdateEngine Gen1RateUpdateEngine;

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_PROTOCOL_RATE_UPDATE_ENGINE_H_
