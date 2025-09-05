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

#ifndef ISEKAI_HOST_FALCON_FALCON_COMPONENT_INTERFACES_H_
#define ISEKAI_HOST_FALCON_FALCON_COMPONENT_INTERFACES_H_

#include <sys/types.h>

#include <cstdint>
#include <memory>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_bitmap.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_counters.h"
#include "isekai/host/falcon/falcon_histograms.h"
#include "isekai/host/falcon/falcon_resource_credits.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_types.h"

namespace isekai {

// The declarations of these interfaces help avoiding cyclic build dependencies.

class ConnectionStateManager {
 public:
  virtual ~ConnectionStateManager() {}
  // Initializes the state corresponding to a new connection based on the ID and
  // metadata provided.
  virtual absl::Status InitializeConnectionState(
      std::unique_ptr<ConnectionMetadata> connection_metadata) = 0;
  // Returns the metadata associated with a given connection instantly. This API
  // is typically used by the various internal modules of FALCON.
  virtual absl::StatusOr<ConnectionState*> PerformDirectLookup(
      uint32_t source_connection_id) const = 0;
};

// Handles the allocation/freeing up of FALCON resources as we get transactions
// and responses from the ULP and network respectively.
class ResourceManager {
 public:
  virtual ~ResourceManager() {}
  // Reserves the necessary TX and/or RX resources for the packet, if required,
  // as defined by the flow control policy, or verifies resource availability.
  virtual absl::Status VerifyResourceAvailabilityOrReserveResources(
      uint32_t scid, const Packet* packet, PacketDirection direction,
      bool reserve_resources) = 0;
  // Releases the necessary TX and/or RX resources for the transaction, if
  // required, as defined by the flow control policy
  virtual absl::Status ReleaseResources(uint32_t scid,
                                        const TransactionKey& transaction_key,
                                        falcon::PacketType type) = 0;
  // Returns the 5-bit quantized value of the most occupied resource of the
  // network region resource pool.
  virtual uint16_t GetNetworkRegionOccupancy() const = 0;
  virtual FalconResourceCredits GetAvailableResourceCredits() const = 0;
};

class InterHostRxScheduler {
 public:
  virtual ~InterHostRxScheduler() {}
  // Enqueues the callback into the host specific Rx queue.
  virtual void Enqueue(uint8_t bifurcation_id,
                       absl::AnyInvocable<void()> cb) = 0;
  // Returns true if the host scheduler has outstanding work.
  virtual bool HasWork() = 0;
  // Performs one unit of work from the host scheduler.
  virtual void ScheduleWork() = 0;
  // Set Xoff/Xon for the given host Rx queue.
  virtual void SetXoff(uint8_t bifurcation_id, bool xoff) = 0;
  // Update the scheduler inter-packet gap based on the size of the packet being
  // sent to ULP.
  virtual void UpdateInterPacketGap(uint32_t packet_size) = 0;
};

// Scheduler interface within FALCON. Represents the interface for both
// first-time transmissions (connection scheduler) and retransmissions
// (retransmission scheduler).
class Scheduler {
 public:
  virtual ~Scheduler() {}
  // Initializes the various connection scheduler queues for given scid.
  virtual absl::Status InitConnectionSchedulerQueues(uint32_t scid) = 0;
  // Adds a packet to the relevant queue for transmitting over the network.
  virtual absl::Status EnqueuePacket(uint32_t scid, uint32_t rsn,
                                     falcon::PacketType type) = 0;
  // Removes a packet from the relevant queue for transmitting over the network.
  virtual absl::Status DequeuePacket(uint32_t scid, uint32_t rsn,
                                     falcon::PacketType type) = 0;
  // Returns true if the connection scheduler has outstanding work.
  virtual bool HasWork() = 0;
  // Performs one unit of work from the connection scheduler.
  virtual bool ScheduleWork() = 0;
  // Returns the queue length for a connection across all packet type queues.
  virtual uint32_t GetConnectionQueueLength(uint32_t scid) = 0;
  // Returns the number of outstanding packets corresponding to a packet queue
  // type across all connections.
  virtual uint64_t GetQueueTypeBasedOutstandingPacketCount(
      PacketTypeQueue queue_type) = 0;
  // Updates the transmission eligibility of packet types and connections.
  // Called when any gating variables are updated.
  virtual void RecomputeEligibility(uint32_t scid) = 0;
};

// Arbitrates between the connection scheduler and retransmission scheduler to
// decide who gets the chance to hand work to the sliding window layer.
class Arbiter {
 public:
  virtual ~Arbiter() {}
  // Schedules the arbiter only when there is outstanding work in connection
  // scheduler or retransmission scheduler.
  virtual void ScheduleSchedulerArbiter() = 0;
  // Returns true if either the connection or retransmission scheduler has
  // outstanding work.
  virtual bool HasWork() = 0;
};

// Owns the ACK, NACK, PiggybackACK, and congestion control metadata storage and
// reflection functionalities in Falcon. Uses AckCoalescingKey as its key for
// state management.
//
// construct to the ACK coalescing engine.
class AckCoalescingEngineInterface {
 public:
  virtual ~AckCoalescingEngineInterface() {}
  // Creates a new coleascing entry for the AckCoalescingKey. If an entry with
  // the provided key exists, exits with a fatal error. void* represents a
  // pointer to the right type of AckCoalescingKey according to Falcon version
  // (Gen1, Gen2, etc.).
  virtual void CreateAckCoalescingEntry(const void* key,
                                        const ConnectionMetadata& metadata) = 0;
  // Handles udpates to the coalescing state when receiving a packet from the
  // network or a ULP ack for the provided AckCoalescingKey.
  virtual absl::Status GenerateAckOrUpdateCoalescingState(
      std::unique_ptr<AckCoalescingKey> key, bool immediate_ack_req,
      int count_increment) = 0;
  // Transmits an ACK for the provided AckCoalescingKey and manages resulting
  // state updates.
  virtual void TransmitACK(const AckCoalescingKey& key, bool req_own,
                           bool data_own) = 0;
  // Transmits a NACK for the provided AckCoalescingKey and manages resulting
  // state updates.
  virtual absl::Status TransmitNACK(
      const AckCoalescingKey& key, uint32_t original_psn,
      bool is_request_window, falcon::NackCode falcon_nack_code,
      const UlpNackMetadata* ulp_nack_metadata) = 0;
  // Piggybacks an ACK for the provided AckCoalescingKey on a data/request
  // packet and handles resulting state updates.
  virtual absl::Status PiggybackAck(const AckCoalescingKey& key,
                                    Packet* packet) = 0;
  // Updates the stored congestion control metadata (to be reflected in an ACK)
  // for the provided AckCoalescingKey upon receiving a new data/request packet.
  virtual absl::Status UpdateCongestionControlMetadataToReflect(
      const AckCoalescingKey& key, const Packet* packet) = 0;
  // Generates an AckCoalescingKey of the right generation type from an incoming
  // packet.
  virtual std::unique_ptr<AckCoalescingKey>
  GenerateAckCoalescingKeyFromIncomingPacket(const Packet* packet) = 0;
  // Generates an AckCoalescingKey of the right generation type from an scid
  // value and an OpaqueCookie reflected back from the ULP.
  virtual std::unique_ptr<AckCoalescingKey> GenerateAckCoalescingKeyFromUlp(
      uint32_t scid, const OpaqueCookie& cookie) = 0;
};

class AdmissionControlManager {
 public:
  virtual ~AdmissionControlManager() {}
  // Checks if the admission control criteria is met.
  virtual bool MeetsAdmissionControlCriteria(uint32_t scid, uint32_t rsn,
                                             falcon::PacketType type) = 0;
  // Checks if the admission control criteria is met for a transaction size
  // given packet type and connection state.
  virtual bool MeetsAdmissionControlCriteria(
      uint64_t request_length, falcon::PacketType type,
      const ConnectionState* connection_state = nullptr) = 0;
  // Reserves capacity related to admission control.
  virtual absl::Status ReserveAdmissionControlResource(
      uint32_t scid, uint32_t rsn, falcon::PacketType type) = 0;
  // Refunds any reserved capacity related to admission control.
  virtual absl::Status RefundAdmissionControlResource(
      uint32_t scid, const TransactionKey& transaction_key) = 0;
};

// Responsibilities include implementing sliding window, loss recovery via
// retransmissions and handling/generating (N)ACKs.
class PacketReliabilityManager {
 public:
  virtual ~PacketReliabilityManager() {}
  // Initializes the per-connection state stored in the packet reliability
  // manager.
  virtual void InitializeConnection(uint32_t scid) = 0;
  // Transmits the packet picked by the arbiter (via the connection scheduler or
  // retransmission scheduler).
  virtual absl::Status TransmitPacket(uint32_t scid, uint32_t rsn,
                                      falcon::PacketType type) = 0;
  // Receives an incoming packet and performing the
  // sliding-window related processing.
  virtual absl::Status ReceivePacket(const Packet* packet) = 0;
  // Handles RTO reduction by explicitly checking if packets need to be
  // retransmitted.
  virtual absl::Status HandleRtoReduction(uint32_t scid) = 0;
  // Handles ACK received from the local ULP. Corresponds to Pull Request, Push
  // Solicited Data and Push Unsolicited Data.
  virtual absl::Status HandleAckFromUlp(uint32_t scid, uint rsn,
                                        const OpaqueCookie& cookie) = 0;
  // Handles NACK received from the local ULP.
  virtual absl::Status HandleNackFromUlp(uint32_t scid,
                                         const TransactionKey& transaction_key,
                                         UlpNackMetadata* nack_metadata,
                                         const OpaqueCookie& cookie) = 0;
  // Verifies if initial transmission meets Tx gating criteria.
  virtual bool MeetsInitialTransmissionCCTxGatingCriteria(
      uint32_t scid, falcon::PacketType type) = 0;
  // Verifies if initial transmission meets Tx OOW gating criteria.
  virtual bool MeetsInitialTransmissionOowTxGatingCriteria(
      uint32_t scid, falcon::PacketType type) = 0;
  virtual uint32_t GetOpenFcwnd(uint32_t cid, falcon::PacketType type) = 0;
  // Verifies if retransmission meets Tx gating criteria.
  virtual bool MeetsRetransmissionCCTxGatingCriteria(
      uint32_t scid, uint32_t psn, falcon::PacketType type) = 0;
};

// The rate update engine (RUE) uses a congestion control algorithm to determine
// the size of the congestion windows and inter packet gap used for each
// connection. The packet reliability manager updates the RUE with information
// about the events that occur for each connection. The RUE is responsible for
// calculating new values for the connection and updating the connection values.
class RateUpdateEngine {
 public:
  virtual ~RateUpdateEngine() = default;
  // Initializes the congestion control portion of the connection state.
  virtual void InitializeMetadata(CongestionControlMetadata& metadata) = 0;
  // Informs the rate update engine about a received ACK packet.
  virtual void ExplicitAckReceived(const Packet* packet, bool eack,
                                   bool eack_drop) = 0;
  // Informs the rate update engine about a received NACK packet.
  virtual void NackReceived(const Packet* packet) = 0;
  // Informs the rate update engine about a retransmitted packet due to timeout.
  virtual void PacketTimeoutRetransmitted(uint32_t cid, const Packet* packet,
                                          uint8_t retransmit_count) = 0;
  // Informs the rate update engine about a retransmitted packet due to early
  // retransmission.
  virtual void PacketEarlyRetransmitted(uint32_t cid, const Packet* packet,
                                        uint8_t retransmit_count) = 0;
  // Translates between Nanosecond and FalconTimeUnit.
  virtual uint32_t ToFalconTimeUnits(absl::Duration time) const = 0;
  virtual absl::Duration FromFalconTimeUnits(uint32_t time) const = 0;
  // Translates between absl::Duration and TimingWheelTimeUnit.
  virtual uint32_t ToTimingWheelTimeUnits(absl::Duration time) const = 0;
  virtual absl::Duration FromTimingWheelTimeUnits(uint32_t time) const = 0;
  // Generates a random flow label. This function is added to the interface
  // because the EventResponseFormatAdapter class needs to call it to generate a
  // random flow label when handling a RUE response.
  virtual uint32_t GenerateRandomFlowLabel() const = 0;
};

class BufferReorderEngine {
 public:
  virtual ~BufferReorderEngine() = default;
  // Initializes the buffer reorder engine related connection state.
  virtual absl::Status InitializeConnection(uint32_t cid,
                                            uint32_t initiator_rsn,
                                            uint32_t target_rsn,
                                            uint32_t initiator_ssn,
                                            uint32_t target_ssn,
                                            OrderingMode ordering_mode) = 0;
  // Deletes a connection, also removing any pending packets in the buffers.
  virtual absl::Status DeleteConnection(uint32_t cid) = 0;
  // Inserts a given packet type into the reorder buffer with given cid, rsn and
  // ssn. The ssn is only relevant for PushRequest and PushGrant packet types.
  // It is ignored for all other packet types.
  virtual absl::Status InsertPacket(falcon::PacketType packet_type,
                                    uint32_t cid, uint32_t rsn,
                                    uint32_t ssn) = 0;
  // Retries an RNR-NACKed packet upon a duplicate packet. Returns
  // FailedPreconditionError if the conection is ordered and in RNR state and
  // this packet is not HoL.
  virtual absl::Status RetryRnrNackedPacket(uint32_t cid, uint32_t rsn) = 0;
  // Determines if the given RSN reflects a HoL request or not.
  virtual bool IsHeadOfLineNetworkRequest(uint32_t cid, uint32_t rsn) = 0;
  // Handles RNR NACK.
  virtual void HandleRnrNackFromUlp(uint32_t cid, uint32_t rsn,
                                    absl::Duration rnr_timeout) = 0;
  // Acks a transaction from ULP at Target.
  virtual void HandleAckFromUlp(uint32_t cid, uint32_t rsn) = 0;
  // Returns the RNR timeout of a connection.
  virtual absl::Duration GetRnrTimeout(uint32_t cid) = 0;
};

class StatsManager {
 public:
  virtual ~StatsManager() = default;

  virtual FalconConnectionCounters& GetConnectionCounters(uint32_t cid) = 0;
  virtual FalconHostCounters& GetHostCounters() = 0;
  virtual FalconHistogramCollector* GetHistogramCollector() = 0;
  virtual StatisticsCollectionConfig::FalconFlags& GetStatsConfig() = 0;

  virtual void UpdateUlpRxCounters(Packet::Rdma::Opcode opcode,
                                   uint32_t cid) = 0;
  virtual void UpdateUlpTxCounters(Packet::Rdma::Opcode opcode,
                                   uint32_t cid) = 0;
  virtual void UpdateNetworkRxCounters(falcon::PacketType type,
                                       uint32_t cid) = 0;
  virtual void UpdateNetworkTxCounters(falcon::PacketType type, uint32_t cid,
                                       bool is_retransmission,
                                       RetransmitReason retx_reason) = 0;
  virtual void UpdateMaxTransmissionCount(uint32_t attempts) = 0;
  virtual void UpdateRueEventCounters(uint32_t cid, falcon::RueEventType event,
                                      bool eack, bool eack_drop) = 0;
  virtual void UpdateRueDroppedEventCounters(uint32_t cid,
                                             falcon::RueEventType event,
                                             bool eack, bool eack_drop) = 0;
  virtual void UpdateRueResponseCounters(uint32_t cid) = 0;
  virtual void UpdateRueEnqueueAttempts(uint32_t cid) = 0;
  virtual void UpdateNetworkRxDropCounters(falcon::PacketType type,
                                           uint32_t cid, absl::Status) = 0;
  virtual void UpdateSolicitationCounters(uint32_t cid, uint64_t window_bytes,
                                          bool is_release) = 0;
  virtual void UpdateRequestOrDataWindowUsage(WindowType type, uint32_t cid,
                                              uint64_t occupied_bytes) = 0;
  virtual void UpdateAlphaCarvingDynamicLimitCounters(
      uint32_t cid, const FalconCredit& credit_limits) = 0;
  virtual void UpdateResourceCounters(uint32_t cid,
                                      FalconResourceCredits credit,
                                      bool is_release) = 0;
  virtual void UpdateSchedulerCounters(SchedulerTypes scheduler_type,
                                       bool is_dequed) = 0;
  virtual void UpdateIntraConnectionSchedulerCounters(
      uint32_t cid, PacketTypeQueue queue_type, bool is_dequed) = 0;
  virtual void UpdateCwndPauseCounters(uint32_t cid, bool is_paused) = 0;
  virtual void UpdateAcksGeneratedCounterDueToAR(uint32_t cid) = 0;
  virtual void UpdateAcksGeneratedCounterDueToTimeout(uint32_t cid) = 0;
  virtual void UpdateAcksGeneratedCounterDueToCoalescingCounter(
      uint32_t cid) = 0;
  virtual void UpdateInitialTxRsnSeries(uint32_t cid, uint32_t rsn,
                                        falcon::PacketType type) = 0;
  virtual void UpdateRxFromUlpRsnSeries(uint32_t cid, uint32_t rsn,
                                        falcon::PacketType type) = 0;
  virtual void UpdateRetxRsnSeries(uint32_t cid, uint32_t rsn,
                                   falcon::PacketType type,
                                   RetransmitReason retx_reason) = 0;
  virtual void UpdateNetworkAcceptedRsnSeries(uint32_t cid,
                                              uint32_t accepted_rsn,
                                              falcon::PacketType type) = 0;
  virtual void UpdateMaxRsnDistance(uint32_t cid, uint32_t rsn_difference) = 0;
  virtual void UpdatePacketBuilderXoff(bool xoff) = 0;
  virtual void UpdateRdmaXoff(uint8_t bifurcation_id, bool xoff) = 0;
  virtual void UpdatePacketBuilderTxBytes(uint32_t cid,
                                          uint32_t pkt_size_bytes) = 0;
  virtual void UpdatePacketBuilderRxBytes(uint32_t cid,
                                          uint32_t pkt_size_bytes) = 0;
};

// The PacketMetadataTransformer module is where all Tx packets from Falcon
// will pass through just before they are transferred to the traffic shaper.
// This module is responsible for modifying packet metadata to implement
// useful simulation features, e.g. (1) Static routing in the network, (2)
// per-packet added delay at a host before being sent on the wire; useful for
// simulating effect of delay on the system without needing to change the
// network topology.
class PacketMetadataTransformer {
 public:
  virtual ~PacketMetadataTransformer() {}
  // Transfers the Tx Packet belonging the 'scid' connection from Falcon to the
  // PacketMetadataTransformer module.
  virtual void TransferTxPacket(std::unique_ptr<Packet> packet,
                                uint32_t scid) = 0;
};

class FalconModelInterface : public FalconInterface {
 public:
  virtual ~FalconModelInterface() = default;

  virtual int GetVersion() const override = 0;
  virtual void InitiateTransaction(std::unique_ptr<Packet> packet) override = 0;
  virtual void TransferRxPacket(std::unique_ptr<Packet> packet) override = 0;
  virtual void AckTransaction(
      uint32_t scid, uint32_t rsn, Packet::Syndrome ack_code,
      absl::Duration rnr_timeout,
      std::unique_ptr<OpaqueCookie> cookie) override = 0;
  virtual uint32_t SetupNewQp(uint32_t scid, QpId qp_id, QpType qp_type,
                              OrderingMode ordering_mode) override = 0;
  virtual absl::Status EstablishConnection(
      uint32_t scid, uint32_t dcid, uint8_t source_bifurcation_id,
      uint8_t destination_bifurcation_id, absl::string_view dst_ip_address,
      OrderingMode ordering_mode,
      const FalconConnectionOptions& connection_options) override = 0;
  virtual const FalconConfig* get_config() const override = 0;
  virtual Environment* get_environment() const override = 0;
  virtual std::string_view get_host_id() const override = 0;
  virtual StatisticCollectionInterface* get_stats_collector()
      const override = 0;
  virtual FalconHistogramCollector* get_histogram_collector()
      const override = 0;
  virtual void SetXoffByPacketBuilder(bool xoff) override = 0;
  virtual bool CanSendPacket() const override = 0;
  virtual void SetXoffByRdma(uint8_t bifurcation_id, bool xoff) = 0;
  virtual void UpdateRxBytes(std::unique_ptr<Packet> packet,
                             uint32_t pkt_size_bytes) override = 0;
  virtual void UpdateTxBytes(std::unique_ptr<Packet> packet,
                             uint32_t pkt_size_bytes) override = 0;

  // Reorder engine callback.
  virtual void ReorderCallback(uint32_t cid, uint32_t rsn,
                               falcon::PacketType type) = 0;

  // Getter for a pointer to the RDMA model.
  virtual RdmaFalconInterface* get_rdma_model() const = 0;
  // Getter for a pointer to the traffic shaper.
  virtual TrafficShaperInterface* get_traffic_shaper() const = 0;
  // Getters to Falcon components used by other internal Falcon components to
  // communicate with each other.
  virtual ConnectionStateManager* get_state_manager() const = 0;
  virtual ResourceManager* get_resource_manager() const = 0;
  virtual InterHostRxScheduler* get_inter_host_rx_scheduler() const = 0;
  virtual Scheduler* get_connection_scheduler() const = 0;
  virtual Scheduler* get_retransmission_scheduler() const = 0;
  virtual Scheduler* get_ack_nack_scheduler() const = 0;
  virtual Arbiter* get_arbiter() const = 0;
  virtual AdmissionControlManager* get_admission_control_manager() const = 0;
  virtual PacketReliabilityManager* get_packet_reliability_manager() const = 0;
  virtual RateUpdateEngine* get_rate_update_engine() const = 0;
  virtual BufferReorderEngine* get_buffer_reorder_engine() const = 0;
  virtual AckCoalescingEngineInterface* get_ack_coalescing_engine() const = 0;
  virtual StatsManager* get_stats_manager() const = 0;
  virtual PacketMetadataTransformer* get_packet_metadata_transformer()
      const = 0;
};

template <typename T>
class PathSelectionPolicy {
 public:
  virtual ~PathSelectionPolicy() {}
  // Initializes an entity in the policy.
  virtual absl::Status InitializeEntity(const T& entity) = 0;
  // Removes an entity from the policy and deletes all associated state.
  virtual absl::Status DeleteEntity(const T& entity) = 0;
  // Returns the next entity to be serviced as dictated by the policy.
  virtual absl::StatusOr<T> GetNextEntity() = 0;
  // Returns true if the policy has an entity that can perform work.
  virtual bool HasWork() = 0;
  // Resets all entities in the policy.
  virtual void Reset() = 0;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_COMPONENT_INTERFACES_H_
