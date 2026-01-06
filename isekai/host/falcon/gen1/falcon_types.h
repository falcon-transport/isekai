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

#ifndef ISEKAI_HOST_FALCON_FALCON_TYPES_H_
#define ISEKAI_HOST_FALCON_FALCON_TYPES_H_

#include <assert.h>

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "isekai/common/constants.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_bitmap.h"
#include "isekai/host/falcon/gating_variable.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/rue/bits.h"

// This file contains definitions of internal Falcon constructs and types.

namespace isekai {

inline constexpr int kIpv6FlowLabelNumBits = 20;
// A connection is limited to 40Mops/s, which translates to a per-connection
// inter op gap of 25ns.
inline constexpr absl::Duration kPerConnectionInterOpGap =
    absl::Nanoseconds(25);
// In hardware, after a packet is retransmitted threshold (say 7) times, it
// notifies software to take the next actions (e.g., teardown connection).
inline constexpr int kDefaultMaxRetransmitAttempts =
    std::numeric_limits<int>::max();
// Set to 64 (smaller of the window sizes), effectively turning off the feature.
inline constexpr int kDefaultEarlyTransmitLimit = 64;
// Suggested by frwang@, used by DV for their performance simulations,
//
static constexpr int kDefaultAckCoalescingThreshold = 10;
static constexpr absl::Duration kDefaultAckCoalescingTimer =
    absl::Microseconds(8);
// ULP-RTO is expected to be >= RTO.
inline constexpr absl::Duration kDefaultUlpRto = absl::Milliseconds(5);

inline constexpr uint8_t kDefaultBifurcationId = 0;

// List of Falcon transaction types.
enum class TransactionType {
  kPull,
  kPushSolicited,
  kPushUnsolicited,
};

// Indicates the location of a transaction. The individual packets are handled
// according to the transaction location, i.e., the state machine is updated
// based on location.
enum class TransactionLocation {
  kInitiator,
  kTarget,
};

// Represents the stage of a transaction. Used to update state of a transaction.
// States of other stages such as UlpRx and NtwkRx is updated implicitly when
// the transaction metadata is created.
enum class TransactionStage {
  UlpRx,            // Represents receiving a transaction from ULP.
  UlpTx,            // Represents sending a transaction/completion to ULP.
  UlpAckRx,         // Represents receiving an ACK from ULP.
  UlpRnrRx,         // Represents receiving an RNR NACK from ULP.
  UlpCompletionTx,  // Represents completion to ULP, for Push transactions only.
  NtwkRx,     // Represents Falcon receiving a transaction from the network.
  NtwkTx,     // Represents Falcon sending a transaction to network.
  NtwkAckRx,  // Represents receiving an ACK from network.
};

// Represents the possible states of a transaction (both at the initiator as
// well as target).
enum class TransactionState {
  kInvalid,
  // Falcon received transactions from ULP.
  kPullReqUlpRx,
  kPullDataUlpRx,
  kPushSolicitedReqUlpRx,
  kPushUnsolicitedReqUlpRx,
  // Falcon transmitted transactions and completion to ULP.
  kPullReqUlpTx,
  kPullDataUlpTx,
  kPushSolicitedDataUlpTx,
  kPushUnsolicitedDataUlpTx,
  kPushCompletionUlpTx,
  // Falcon received ACK from ULP.
  kPullReqAckUlpRx,
  kPushSolicitedDataAckUlpRx,
  kPushUnsolicitedDataAckUlpRx,
  // Falcon received RNR NACK from ULP.
  kPullReqRnrUlpRx,
  kPushSolicitedDataRnrUlpRx,
  kPushUnsolicitedDataRnrUlpRx,
  // Falcon received transactions from network.
  kPullReqNtwkRx,
  kPullDataNtwkRx,
  kPushReqNtwkRx,
  kPushGrantNtwkRx,
  kPushSolicitedDataNtwkRx,
  kPushUnsolicitedDataNtwkRx,
  // Falcon transmitted transactions to network.
  kPullReqNtwkTx,
  kPullDataNtwkTx,
  kPushReqNtwkTx,
  kPushGrantNtwkTx,
  kPushSolicitedDataNtwkTx,
  kPushUnsolicitedDataNtwkTx,
  // Falcon received ACK from network.
  kPullDataAckNtwkRx,
  kPushSolicitedDataAckNtwkRx,
  kPushUnsolicitedDataAckNtwkRx,
};

// Indicates the direction of a packet within a transaction.
enum class PacketDirection {
  kOutgoing,
  kIncoming,
};

// Describes the reason a packet is being retransmitted.
enum class RetransmitReason : uint8_t {
  kTimeout = 0,
  // For OOO-distance triggered retransmission.
  kEarlyOooDis = 1,
  // For RACK triggered retransmission.
  kEarlyRack = 2,
  // For TLP triggered retransmission.
  kEarlyTlp = 3,
  // For early retransmission triggered by out-of-sequence NACK.
  kEarlyNack = 4,
  // For retransmission of received packet -- this indicates some thing from
  // target ULP is missing, so this is for recovery of ULP ACK/NACK, hence
  // called ULP-RTO.
  kUlpRto = 5,
};

// Structure to hold metadata from the ULP about the NACK.
struct UlpNackMetadata {
  Packet::Syndrome ulp_nack_code;
  absl::Duration rnr_timeout = absl::ZeroDuration();
};

// The structure of an ack coalescing key, which contains the scid of the
// connection.
struct AckCoalescingKey {
  uint32_t scid = 0;

  explicit AckCoalescingKey(uint32_t scid) : scid(scid) {}
  AckCoalescingKey() {}
  virtual ~AckCoalescingKey() {}
  template <typename H>
  friend H AbslHashValue(H state, const AckCoalescingKey& value) {
    state = H::combine(std::move(state), value.scid);
    return state;
  }
  // Two AckCoalescingKey structs are equal if they share the same scid.
  inline bool operator==(const AckCoalescingKey& other) const {
    return other.scid == scid;
  }
};

// The structure of a transaction key, which contains the rsn and location of
// the transaction to uniquely identify and access a transaction in Falcon.
struct TransactionKey {
  uint32_t rsn;
  TransactionLocation location;

  template <typename H>
  friend H AbslHashValue(H h, const TransactionKey& s) {
    return H::combine(std::move(h), s.rsn, s.location);
  }

  inline bool operator==(const TransactionKey& s) const {
    return (rsn == s.rsn && location == s.location);
  }

  TransactionKey(uint32_t rsn, TransactionLocation location)
      : rsn(rsn), location(location) {}
  TransactionKey() {}
};

// The structure of a UlpTransaction, containing the rsn and the type of the
// transaction packet received from ULP.
struct UlpTransactionInfo {
  uint32_t rsn;
  falcon::PacketType type;

  template <typename H>
  friend H AbslHashValue(H h, const UlpTransactionInfo& s) {
    return H::combine(std::move(h), s.rsn, s.type);
  }

  inline bool operator==(const UlpTransactionInfo& s) const {
    return (rsn == s.rsn && type == s.type);
  }

  UlpTransactionInfo(uint32_t rsn, falcon::PacketType type)
      : rsn(rsn), type(type) {}
  UlpTransactionInfo() {}
};

// The structure contains the expected and actual length of the transaction
// received from the network.
struct NetworkTransactionInfo {
  uint16_t expected_length;
  uint16_t actual_length;

  template <typename H>
  friend H AbslHashValue(H h, const NetworkTransactionInfo& s) {
    return H::combine(std::move(h), s.expected_length, s.actual_length);
  }

  inline bool operator==(const NetworkTransactionInfo& s) const {
    return (expected_length == s.expected_length &&
            actual_length == s.actual_length);
  }

  NetworkTransactionInfo(uint16_t expected_length, uint16_t actual_length)
      : expected_length(expected_length), actual_length(actual_length) {}
  NetworkTransactionInfo() {}
};

typedef FalconBitmap<kRxBitmapWidth> FalconRxBitmap;
typedef FalconBitmap<kTxBitmapWidth> FalconTxBitmap;
typedef FalconBitmap<kGen2RxBitmapWidth> Gen2FalconRxBitmap;
typedef FalconBitmap<kGen2TxBitmapWidth> Gen2FalconTxBitmap;
typedef FalconBitmap<kGen3RxBitmapWidth> Gen3FalconRxBitmap;
typedef FalconBitmap<kGen3TxBitmapWidth> Gen3FalconTxBitmap;
typedef AckCoalescingKey RueKey;

// Stores metadata for a Falcon packet withing a pending transaction.
struct PacketMetadata {
  uint32_t psn;
  falcon::PacketType type;
  // Represents the direction of the packet.
  PacketDirection direction;
  // Represents if the transaction packet holds resources currently or not.
  absl::Status resources_reserved = absl::OkStatus();
  // Represents the number of transmission attempts of this packet, including
  // both early and timeout based retransmissions.
  uint32_t transmit_attempts = 0;
  // Represents the number of timeout based retransmissions.
  uint32_t timeout_retransmission_attempts = 0;
  // Represents the number of times this packet is early-retx'ed.
  uint32_t early_retx_attempts = 0;
  // Represents the reason of retransmission of the packet.
  RetransmitReason retransmission_reason = RetransmitReason::kTimeout;
  // Represents the total length of CRTBTH and other RDMA headers and any inline
  // data if present (does not include SGL length).
  uint32_t data_length = 0;
  // Represents the SGL length in the TX PMD.
  uint32_t sgl_length = 0;
  // Represents the Falcon payload length (if an incoming packet).
  uint32_t payload_length = 0;
  // Represents the time when the packet was transmitted.
  absl::Duration transmission_time = absl::ZeroDuration();
  // Maintains a packet copy to trigger retransmissions.
  Packet retransmission_packet_copy;
  std::unique_ptr<Packet> active_packet;
  // Represents if the solicitation layer has attempted to schedule the
  // transaction and hand it over to the sliding window layer.
  absl::Status schedule_status = absl::UnknownError("Not scheduled.");
  // Counters related to queuing in the connection Scheduler.
  struct PacketQueuingDelayMetadata {
    // Represents the time when the packet is queued at the scheduler.
    absl::Duration enqueue_time = absl::ZeroDuration();
    // Represents the time when the packet becomes TX-eligible.
    // Push solicited data becomes TX-eligible when receiving the push grant.
    // Unsolicited data becomes TX-eligible when the phantom request is
    // scheduled.
    absl::Duration tx_eligible_time = absl::ZeroDuration();
    // Snapshot of connection scheduler packet type-wise TX counters when this
    // packet is enqueued. These are compared against the counters later when
    // this packet is scheduled to calculate how many packets *per type* are
    // scheduled during the queuing of this packet.
    absl::flat_hash_map<falcon::PacketType, uint64_t>
        enqueue_packet_type_wise_tx_count_snapshot;
    // Snapshot of connection shceduler queue-wise TX counters when this
    // packet is enqueued. These are compared against the counters later when
    // this packet is scheduled to calculate how many packets *per queue* are
    // scheduled during the queuing of this packet.
    absl::flat_hash_map<PacketTypeQueue, uint64_t>
        enqueue_packet_queue_wise_pkt_count_snapshot;
  } queuing_delay_metadata;
  // Represents if the packet is eligible for transmission or not.
  // In ordered connections, Solicited Push Data packets are not eligible for
  // scheduling until the corresponding grant is received (as they are enqueued
  // along with Push Request into the connection scheduler to maintain ordering
  // between solicited and unsolicited data which is required to prevent
  // deadlocks).
  bool is_transmission_eligible = true;
  // If this packet is received or not.
  bool received = false;
  // Represents if the packet has been NACKed by the ULP.
  falcon::NackCode nack_code = falcon::NackCode::kNotANack;
  UlpNackMetadata ulp_nack_metadata;
  // This flag is required for EACK-OWN related changes. The flag is set to true
  // when a packet is created and when a packet is timeout-based retransmitted.
  // The bypass is disallowed, i.e., flag is set to false, once a packet had
  // been early retransmitted.
  bool bypass_recency_check = true;
  // This flag is required for EACK-OWN related changes. The flag is set to true
  // when a packet is early retransmistted as a consequence of serving an EACK.
  // The bypass is disallowed, i.e., flag set to false when -
  // (a) Packet is early retransmitted while servicing EACK-OWN, or
  // (b) Packet bypasses the scanning exit criteria, or
  // (c) Packet is timeout-based retransmitted.
  bool bypass_scanning_exit_criteria = false;
  PacketMetadata() {}
  // By default all packets are eligible to be considered for transmission by
  // the connection scheduler, leaving ordered push solicited data.
  PacketMetadata(falcon::PacketType _type, PacketDirection _direction,
                 std::unique_ptr<Packet> _packet,
                 bool _is_transmission_eligible)
      : type(_type),
        direction(_direction),
        active_packet(std::move(_packet)),
        is_transmission_eligible(_is_transmission_eligible) {}
};

// Represents generic transaction metadata.
struct TransactionMetadata {
  uint32_t rsn;
  uint32_t request_length;
  TransactionType type;
  TransactionLocation location;
  TransactionState state = TransactionState::kInvalid;
  // Represents if resources have been reserved proactively for the entire
  // transaction.
  absl::Status resources_reserved = absl::OkStatus();
  // Enables capturing solicitation window occupancy latency.
  absl::Duration solicitation_window_reservation_timestamp;
  // Holds the metadata corresponding to the individual packets that make up
  // this transaction, keyed by the packet type.
  absl::flat_hash_map<falcon::PacketType, std::unique_ptr<PacketMetadata>>
      packets;

  // Creates a packet for this transaction.
  void CreatePacket(falcon::PacketType type, PacketDirection direction,
                    std::unique_ptr<Packet> packet,
                    bool is_transmission_eligible = true) {
    packets[type] = std::make_unique<PacketMetadata>(
        type, direction, std::move(packet), is_transmission_eligible);
  }
  // Creates a packet and sets the data/sgl length (typically done for ULP
  // generated packets).
  void CreatePacket(falcon::PacketType type, PacketDirection direction,
                    uint32_t data_length, uint32_t sgl_length,
                    std::unique_ptr<Packet> packet,
                    bool is_transmission_eligible = true) {
    CreatePacket(type, direction, std::move(packet), is_transmission_eligible);
    packets[type]->data_length = data_length;
    packets[type]->sgl_length = sgl_length;
  }
  // Creates a packet and sets the Falcon payload length (typically done for
  // network generated transactions).
  void CreatePacket(falcon::PacketType type, PacketDirection direction,
                    uint32_t payload_length, std::unique_ptr<Packet> packet,
                    bool is_transmission_eligible = true) {
    packets[type] = std::make_unique<PacketMetadata>(
        type, direction, std::move(packet), is_transmission_eligible);
    packets[type]->payload_length = payload_length;
  }
  // Returns the packet metadata based on packet type.
  absl::StatusOr<PacketMetadata*> GetPacketMetadata(falcon::PacketType type) {
    auto it = packets.find(type);
    if (it != packets.end()) {
      return it->second.get();
    }
    return absl::NotFoundError("Packet not found.");
  }
  // Updates the state of the transaction based on the packet type and the
  // current stage of the transaction.
  void UpdateState(falcon::PacketType type, TransactionStage stage,
                   FalconInterface* falcon);
  TransactionMetadata() {}
  TransactionMetadata(uint32_t rsn, TransactionType ctype,
                      TransactionLocation location)
      : rsn(rsn), type(ctype), location(location) {}
  virtual ~TransactionMetadata() {}

 private:
  absl::Duration transaction_start_timestamp;  // Enables capturing transaction
                                               // end-to-end latency.
  absl::Duration last_state_update_timestamp;  // Enables capturing transaction
                                               // stage latencies.
};

// Stores the context of the received packet within the sliding window layer
// (referred to as rx_cmd_ctx in the Falcon MAS). The lifetime of this context
// is as follows -
// 1. Created (if does not exist) when sending a packet out and when receiving a
// packet.
// 2. Deleted when ULP ACKs (at the target) and when completions are delivered
// to ULP at the initiator.
struct ReceivedPacketContext {
  // Stores the local QP ID as this is required to send a completion (including
  // ULP NACK) to the ULP.
  uint32_t qp_id = 0;
  // Stores the PSN of the received packet as this is required when a ULP NACK
  // needs to be sent out.
  uint32_t psn = 0;
  // Stores the type of the packet received as this is required when a ULP NACK
  // needs to be sent out.
  falcon::PacketType type;
  // Stored for incoming packets with AR-bit set so that we can send back an ACK
  // after ULP acks the packet.
  bool is_ack_requested = false;
  ReceivedPacketContext() {}
  virtual ~ReceivedPacketContext() {}
};

// Stores the metadata related to the receiver reliability window (e.g., data
// window or request window)
struct ReceiverReliabilityWindowMetadata {
  uint32_t base_packet_sequence_number = 0;
  uint32_t next_packet_sequence_number = 0;
  // Indicates if this window has experienced sliding window drops. It is reset
  // once an ACK is sent out on this connection conveying this information.
  bool has_oow_drop = false;
  //
  // Bitmap to indicate if a packet has been received by Falcon.
  std::unique_ptr<FalconBitmapInterface> receive_window;
  // Bitmap to indicate if a packet has been acknowledged by Falcon. For packet
  // types of Push Solicited Data and Push Unsolicited Data, the ULP needs to
  // send out an explicit ACK for us to change the corresponding bit to 1. For
  // other packet types, this bit is set to 1 when Falcon receives the packet.
  std::unique_ptr<FalconBitmapInterface> ack_window;
};

// Stores the context of the outstanding packet within the sliding window layer.
struct OutstandingPacketContext {
  uint32_t rsn = 0;
  falcon::PacketType packet_type;
  OutstandingPacketContext() {}
  OutstandingPacketContext(uint32_t rsn, falcon::PacketType type)
      : rsn(rsn), packet_type(type) {}
  virtual ~OutstandingPacketContext() {}
};

// Stores the metadata related to the transmitter reliability window (e.g., data
// window or request window)
struct TransmitterReliabilityWindowMetadata {
  static const int kTransmitterRequestWindowSize = 1024;
  static const int kTransmitterDataWindowSize = 1024;
  WindowType type;
  GatingVariable<uint32_t> base_packet_sequence_number;
  GatingVariable<uint32_t> next_packet_sequence_number;
  GatingVariable<uint32_t> outstanding_requests_counter;
  // This variable and its behavior differs from the HW implementation, and is
  // not necessary in a HW implementation.
  GatingVariable<uint32_t> outstanding_retransmission_requests_counter;
  GatingVariable<bool> oow_notification_received;
  std::unique_ptr<FalconBitmapInterface> window;
  // Set that prioritizes outstanding packets with lower PSN.
  absl::btree_set<RetransmissionWorkId> outstanding_packets;
  // Mapping between PSN and outstanding packet context (consists of RSN, etc).
  absl::flat_hash_map<uint32_t, std::unique_ptr<OutstandingPacketContext>>
      outstanding_packet_contexts;
  absl::StatusOr<const OutstandingPacketContext* const> GetOutstandingPacketRSN(
      uint32_t psn) const {
    auto it = outstanding_packet_contexts.find(psn);
    if (it != outstanding_packet_contexts.end()) {
      return (it->second).get();
    }
    return absl::NotFoundError("OutstandingPacketContext not found.");
  }
  explicit TransmitterReliabilityWindowMetadata(WindowType type) : type(type) {}
};

struct ConnectionMetadata {
  uint32_t scid;
  uint32_t dcid;
  std::string dst_ip = "0:0:0:0:0:0:0:0";
  enum class RdmaMode {
    kRC,
    kUD,
  } rdma_mode = RdmaMode::kRC;
  OrderingMode ordered_mode = OrderingMode::kOrdered;
  uint32_t max_retransmit_attempts = kDefaultMaxRetransmitAttempts;
  uint32_t early_retransmit_limit = kDefaultEarlyTransmitLimit;
  uint32_t ack_coalescing_threshold = kDefaultAckCoalescingThreshold;
  absl::Duration ack_coalescing_timeout = kDefaultAckCoalescingTimer;
  uint8_t source_bifurcation_id = kDefaultBifurcationId;
  uint8_t destination_bifurcation_id = kDefaultBifurcationId;

  // This optional vector (static_routing_port_lists) includes the static
  // routing port lists for every path, indexed by path ID. Each element
  // specifies the router ports that packets on this path should traverse
  // sequentially, either in the forward or reverse direction. The number of
  // elements in the static_routing_port_lists must be equal to the degree of
  // multipathing. Non-multipathing cases are also supported, as the degree of
  // multipathing is 1 by default.
  // This variable is used only for specific simulation tests that aim to
  // enable static routing.
  std::optional<std::vector<std::vector<uint32_t>>> static_routing_port_lists;
  ConnectionMetadata() {}
  virtual ~ConnectionMetadata() {}
};

// Stores all metadata related to congestion control.
// see //isekai/host/falcon/rue/format.h
struct CongestionControlMetadata {
  virtual ~CongestionControlMetadata() = default;

  // DowncastTo() is a helper function to cast descendant classes of
  // CongestionControlMetadata from the base class.
  template <typename ChildT>
  inline static ChildT& DowncastTo(CongestionControlMetadata& ccmeta) {
    static_assert((std::is_base_of<CongestionControlMetadata,
                                   std::remove_reference_t<ChildT>>::value),
                  "target type not derived from CongestionControlMetadata");
    ChildT* child = dynamic_cast<ChildT*>(&ccmeta);
    CHECK_NE(child, nullptr);  // Crash OK
    return *child;
  }

  // Same as above, but for const CongestionControlMetadata.
  template <typename ChildT>
  inline static const ChildT& DowncastTo(
      const CongestionControlMetadata& ccmeta) {
    static_assert((std::is_base_of<CongestionControlMetadata,
                                   std::remove_reference_t<ChildT>>::value),
                  "target type not derived from CongestionControlMetadata");
    const ChildT* child = dynamic_cast<const ChildT*>(&ccmeta);
    CHECK_NE(child, nullptr);  // Crash OK
    return *child;
  }

  // These values are used by various FALCON components
  uint32_t flow_label : kIpv6FlowLabelNumBits;
  GatingVariable<uint32_t> fabric_congestion_window;
  absl::Duration inter_packet_gap;
  GatingVariable<uint32_t> nic_congestion_window;
  absl::Duration retransmit_timeout;
  // These values are only used in the RUE
  uint32_t cc_metadata : falcon_rue::kCcMetadataBits;
  uint32_t fabric_window_time_marker : falcon_rue::kTimeBits;
  uint32_t nic_window_time_marker : falcon_rue::kTimeBits;
  falcon::WindowDirection nic_window_direction
      : falcon_rue::kWindowDirectionBits;
  falcon::DelaySelect delay_select : falcon_rue::kDelaySelectBits;
  uint32_t base_delay : falcon_rue::kTimeBits;
  uint32_t delay_state : falcon_rue::kTimeBits;
  uint32_t rtt_state : falcon_rue::kTimeBits;
  uint8_t cc_opaque : falcon_rue::kGen1CcOpaqueBits;
  uint8_t rx_buffer_level : falcon_rue::kRxBufferLevelBits;
  // Number of packets ACKed since last successful RUE event.
  uint32_t num_acked;
  // Set when posting RUE event, reset when the RUE response is processed.
  bool outstanding_rue_event;
  // Record of the time of the last successful RUE event.
  absl::Duration last_rue_event_time;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_TYPES_H_
