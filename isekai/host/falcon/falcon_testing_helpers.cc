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

#include "isekai/host/falcon/falcon_testing_helpers.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/connection_state_utils.h"
#include "isekai/host/falcon/event_response_format_adapter.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/ack_coalescing_engine.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_model.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/packet_reliability_manager.h"
#include "isekai/host/falcon/gen1/rate_update_engine.h"
#include "isekai/host/falcon/gen1/rate_update_engine_adapter.h"
#include "isekai/host/falcon/gen2/ack_coalescing_engine.h"
#include "isekai/host/falcon/gen2/falcon_model.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/falcon/gen2/falcon_utils.h"
#include "isekai/host/falcon/rue/format.h"
#include "isekai/host/falcon/rue/format_gen2.h"
#include "isekai/host/rnic/connection_manager.h"

namespace isekai {

void FalconTestingHelpers::FalconTestSetup::InitFalcon(
    const FalconConfig& config) {
  if (config.version() == 1) {
    falcon_ = std::make_unique<FalconModel>(
        config, &env_, /*stats_collector=*/nullptr,
        ConnectionManager::GetConnectionManager(), "falcon-host",
        /* number of hosts */ 4);
  } else if (config.version() == 2) {
    // If Gen2, use 4 flows (paths) per connection.
    falcon_ = std::make_unique<Gen2FalconModel>(
        config, &env_, /*stats_collector=*/nullptr,
        ConnectionManager::GetConnectionManager(), "falcon-host",
        /* number of hosts */ 4);
  } else {
    LOG(FATAL) << "Unsupported Falcon version: " << config.version();
  }
  falcon_->ConnectShaper(&shaper_);
  falcon_->ConnectRdma(&rdma_);
  connection_state_manager_ = falcon_->get_state_manager();
  reliability_manager_ = falcon_->get_packet_reliability_manager();
  ack_coalescing_engine_ = falcon_->get_ack_coalescing_engine();
  admission_control_manager_ = falcon_->get_admission_control_manager();
  reorder_engine_ = falcon_->get_buffer_reorder_engine();
  connection_scheduler_ = falcon_->get_connection_scheduler();
  stats_manager_ = falcon_->get_stats_manager();
  resource_manager_ = falcon_->get_resource_manager();
}

// Creates the right version of OpaqueCookie. flow_id argument is
// only used for Gen2.
std::unique_ptr<OpaqueCookie>
FalconTestingHelpers::FalconTestSetup::CreateOpaqueCookie(uint32_t scid,
                                                          uint32_t flow_id) {
  int falcon_version = falcon_->get_config()->version();
  std::unique_ptr<OpaqueCookie> cookie;
  if (falcon_version == 1) {
    cookie = std::make_unique<OpaqueCookie>();
  } else if (falcon_version >= 2) {
    cookie = std::make_unique<Gen2OpaqueCookie>(flow_id);
  }
  return cookie;
}

// Creates the right version of AckCoalescingKey. flow_label argument is
// only used for Gen2.
std::unique_ptr<AckCoalescingKey>
FalconTestingHelpers::FalconTestSetup::CreateAckCoalescingKey(
    uint32_t scid, uint32_t flow_label) {
  int falcon_version = falcon_->get_config()->version();
  std::unique_ptr<AckCoalescingKey> ack_coalescing_key;
  if (falcon_version == 1) {
    ack_coalescing_key = std::make_unique<AckCoalescingKey>(scid);
  } else if (falcon_version >= 2) {
    uint8_t flow_id = GetFlowIdFromFlowLabel(flow_label, falcon_.get(), scid);
    ack_coalescing_key = std::make_unique<Gen2AckCoalescingKey>(scid, flow_id);
  }
  return ack_coalescing_key;
}

// Handles adding a packet to the list of outstanding packets by modifying the
// TX window's outstanding_packet_contexts and outstanding_packets. For creating
// a Gen2OutstandingPacketContext, the arguments scid and flow_label are needed
// to calculate the flow_id.
void FalconTestingHelpers::FalconTestSetup::AddOutstandingPacket(
    ConnectionState* connection_state, uint32_t psn, uint32_t rsn,
    falcon::PacketType packet_type, uint32_t scid, uint32_t flow_label) {
  int falcon_version = falcon_->get_config()->version();
  std::unique_ptr<OutstandingPacketContext> packet_context;
  if (falcon_version == 1) {
    packet_context =
        std::make_unique<OutstandingPacketContext>(rsn, packet_type);
  } else if (falcon_version >= 2) {
    uint8_t flow_id = GetFlowIdFromFlowLabel(flow_label, falcon_.get(), scid);
    packet_context = std::make_unique<Gen2OutstandingPacketContext>(
        rsn, packet_type, flow_id);
  }

  auto* tx_window = GetAppropriateTxWindow(
      &connection_state->tx_reliability_metadata, packet_type);
  tx_window->outstanding_packet_contexts[psn] = std::move(packet_context);
  tx_window->outstanding_packets.insert(
      RetransmissionWorkId(rsn, psn, packet_type));
}

// Gets the AckCoalescingEntry corresponding to the AckCoalescingKey, based
// on the version of AckCoalescingEngine.
absl::StatusOr<const AckCoalescingEntry*>
FalconTestingHelpers::FalconTestSetup::GetAckCoalescingEntry(
    const AckCoalescingKey* ack_coalescing_key) {
  int falcon_version = falcon_->get_config()->version();
  absl::StatusOr<const AckCoalescingEntry*> ack_coalescing_entry;
  // Gen2AckCoalescingEngine* cannot be represented by a
  // Gen1AckCoalescingEngine* because Gen2AckCoalescingEngine inherits from
  // AckCoalescingEngine<Gen2AckCoalescingKey>, while
  // Gen1AckCoalescingEngine is AckCoalescingEngine<AckCoalescingKey>.
  if (falcon_version == 1) {
    ack_coalescing_entry =
        dynamic_cast<Gen1AckCoalescingEngine*>(ack_coalescing_engine_)
            ->GetAckCoalescingEntryForTesting(ack_coalescing_key);
  } else if (falcon_version >= 2) {
    ack_coalescing_entry =
        dynamic_cast<Gen2AckCoalescingEngine*>(ack_coalescing_engine_)
            ->GetAckCoalescingEntryForTesting(ack_coalescing_key);
  }
  return ack_coalescing_entry;
}

std::unique_ptr<Packet> FalconTestingHelpers::CreatePacket(
    falcon::PacketType type, uint32_t dest_cid, uint32_t psn, uint32_t rsn,
    bool ack_req, uint32_t flow_label) {
  auto packet = std::make_unique<Packet>();
  packet->packet_type = type;
  packet->falcon.dest_cid = dest_cid;
  packet->falcon.psn = psn;
  packet->falcon.rsn = rsn;
  packet->falcon.ack_req = ack_req;
  packet->metadata.flow_label = flow_label;
  return packet;
}

// Creates EACK packet with the given fields
std::unique_ptr<Packet> FalconTestingHelpers::CreateEackPacket(
    uint32_t dest_cid, uint32_t rdbpsn, uint32_t rrbpsn, absl::Duration t4,
    absl::Duration t1, bool data_own, bool request_own) {
  auto eack_packet = std::make_unique<Packet>();
  eack_packet->packet_type = falcon::PacketType::kAck;
  eack_packet->ack.ack_type = Packet::Ack::kEack;
  eack_packet->ack.dest_cid = dest_cid;
  eack_packet->ack.rdbpsn = rdbpsn;
  eack_packet->ack.rrbpsn = rrbpsn;
  eack_packet->timestamps.received_timestamp = t4;
  eack_packet->ack.timestamp_1 = t1;
  eack_packet->ack.data_own = data_own;
  eack_packet->ack.request_own = request_own;
  return eack_packet;
}

std::pair<Packet*, ConnectionState*> FalconTestingHelpers::SetupTransaction(
    const FalconModel* falcon, TransactionType transaction_type,
    TransactionLocation transaction_location, TransactionState state,
    falcon::PacketType packet_metadata_type, uint32_t scid, uint32_t rsn,
    uint32_t psn, OrderingMode ordering_mode) {
  std::unique_ptr<ConnectionMetadata> metadata =
      InitializeConnectionMetadata(falcon, scid, ordering_mode);

  ConnectionStateManager* const connection_state_manager =
      falcon->get_state_manager();
  // Not checking if a connection with the same scid exists.
  absl::Status status =
      connection_state_manager->InitializeConnectionState(std::move(metadata));
  // Get a handle on the connection state.
  absl::StatusOr<ConnectionState*> connection_state =
      connection_state_manager->PerformDirectLookup(scid);
  EXPECT_OK(connection_state);

  auto packet = SetupTransactionWithConnectionState(
      connection_state.value(), transaction_type, transaction_location, state,
      packet_metadata_type, rsn, psn);
  return {packet, connection_state.value()};
}

std::unique_ptr<ConnectionMetadata>
FalconTestingHelpers::InitializeConnectionMetadata(const FalconModel* falcon,
                                                   uint32_t scid,
                                                   OrderingMode ordering_mode) {
  std::unique_ptr<ConnectionMetadata> connection_metadata;
  if (falcon->get_config()->version() == 1) {
    connection_metadata = std::make_unique<ConnectionMetadata>();
  } else if (falcon->get_config()->version() >= 2) {
    connection_metadata = std::make_unique<Gen2ConnectionMetadata>();
  } else {
    LOG(FATAL) << "Unsupported Falcon version: "
               << falcon->get_config()->version();
  }
  connection_metadata->scid = scid;
  connection_metadata->ordered_mode = ordering_mode;
  connection_metadata->source_bifurcation_id = kSourceBifurcationId;
  connection_metadata->destination_bifurcation_id = kDestinationBifurcationId;

  return connection_metadata;
}

ConnectionState* FalconTestingHelpers::InitializeConnectionState(
    const FalconModel* falcon, std::unique_ptr<ConnectionMetadata> metadata,
    bool expect_ok) {
  // Initialize the connection state with the provided metadata.
  auto scid = metadata->scid;
  ConnectionStateManager* const connection_state_manager =
      falcon->get_state_manager();
  absl::Status status =
      connection_state_manager->InitializeConnectionState(std::move(metadata));
  if (expect_ok) {
    EXPECT_OK(status);
  }
  // Get a handle on the connection state.
  absl::StatusOr<ConnectionState*> connection_state =
      connection_state_manager->PerformDirectLookup(scid);
  EXPECT_OK(connection_state);
  return connection_state.value();
}

std::unique_ptr<ReceivedPacketContext>
FalconTestingHelpers::CreateReceivedPacketContext(uint32_t falcon_version) {
  if (falcon_version == 1) {
    return std::make_unique<ReceivedPacketContext>();
  } else if (falcon_version == 2) {
    return std::make_unique<Gen2ReceivedPacketContext>();
  } else {
    // No changes to received_packet_context for other versions > 2.
    return std::make_unique<Gen2ReceivedPacketContext>();
  }
}

Packet* FalconTestingHelpers::SetupTransactionWithConnectionState(
    ConnectionState* connection_state, TransactionType transaction_type,
    TransactionLocation transaction_location, TransactionState state,
    falcon::PacketType packet_metadata_type, uint32_t rsn, uint32_t psn,
    uint32_t request_length, Packet::Rdma::Opcode rdma_opcode) {
  // Initialize packet metadata.
  auto packet_metadata = std::make_unique<PacketMetadata>();
  packet_metadata->psn = psn;
  packet_metadata->type = packet_metadata_type;
  packet_metadata->direction = PacketDirection::kOutgoing;
  packet_metadata->active_packet = std::make_unique<Packet>();
  packet_metadata->active_packet->rdma.request_length = request_length;
  packet_metadata->active_packet->falcon.rsn = rsn;
  packet_metadata->active_packet->metadata.scid =
      connection_state->connection_metadata->scid;
  packet_metadata->active_packet->rdma.opcode = rdma_opcode;
  // Get a handle on the packet.
  auto* packet = packet_metadata->active_packet.get();

  // Initialize the transaction metadata.
  auto transaction = CreateTransactionMetadata(
      connection_state->version, rsn, transaction_type, transaction_location);
  transaction->state = state;
  transaction->request_length = request_length;
  transaction->packets[packet_metadata_type] = std::move(packet_metadata);

  if (transaction_type == TransactionType::kPushUnsolicited) {
    auto phantom_request = std::make_unique<PacketMetadata>();
    phantom_request->type = falcon::PacketType::kInvalid;
    transaction->packets[phantom_request->type] = std::move(phantom_request);
  }

  // Add transaction to the connection state.
  connection_state->transactions[{transaction->rsn, transaction_location}] =
      std::move(transaction);

  return packet;
}

//
// incoming transactions.
Packet* FalconTestingHelpers::SetupIncomingTransactionWithConnectionState(
    ConnectionState* connection_state, TransactionType transaction_type,
    TransactionLocation transaction_location, TransactionState state,
    falcon::PacketType packet_metadata_type, uint32_t rsn, uint32_t psn,
    uint32_t request_length, Packet::Rdma::Opcode rdma_opcode) {
  // Initialize packet metadata.
  auto packet_metadata = std::make_unique<PacketMetadata>();
  packet_metadata->psn = psn;
  packet_metadata->type = packet_metadata_type;
  packet_metadata->direction = PacketDirection::kIncoming;
  packet_metadata->active_packet = std::make_unique<Packet>();
  packet_metadata->active_packet->rdma.request_length = request_length;
  packet_metadata->active_packet->falcon.rsn = rsn;
  packet_metadata->active_packet->metadata.scid =
      connection_state->connection_metadata->scid;
  packet_metadata->active_packet->falcon.dest_cid =
      connection_state->connection_metadata->scid;
  packet_metadata->active_packet->rdma.opcode = rdma_opcode;
  // Get a handle on the packet.
  auto* packet = packet_metadata->active_packet.get();

  // Initialize the transaction metadata.
  auto transaction = CreateTransactionMetadata(
      connection_state->version, rsn, transaction_type, transaction_location);
  transaction->state = state;
  transaction->request_length = request_length;
  transaction->packets[packet_metadata_type] = std::move(packet_metadata);

  // Add transaction to the connection state.
  connection_state->transactions[{transaction->rsn, transaction_location}] =
      std::move(transaction);

  return packet;
}

void FalconTestingHelpers::SetMultipathingDegree(
    ConnectionMetadata* connection_metadata, uint8_t degree_of_multipathing) {
  auto gen2_multipath_metadata =
      dynamic_cast<Gen2ConnectionMetadata*>(connection_metadata);
  gen2_multipath_metadata->degree_of_multipathing = degree_of_multipathing;
}

template <typename EventT, typename ResponseT>
void FalconTestingHelpers::FakeRueAdapter<EventT, ResponseT>::ProcessNextEvent(
    uint32_t now) {
  auto event = std::move(event_queue_.front());
  event_queue_.pop();
  response_queue_.push(std::make_unique<ResponseT>());
  response_queue_.back()->connection_id = event->connection_id;
}

// Template specialization for FakeRueAdapter in Gen_2. In Gen_2, we need to
// properly handle multipathing. For example, the response should reflect the
// correct flow ID and for the purpose of testing not update the flow weights in
// the connection state.
template <>
void FalconTestingHelpers::FakeRueAdapter<
    falcon_rue::Event_Gen2,
    falcon_rue::Response_Gen2>::ProcessNextEvent(uint32_t now) {
  auto event = std::move(event_queue_.front());
  event_queue_.pop();
  response_queue_.push(std::make_unique<falcon_rue::Response_Gen2>());
  response_queue_.back()->connection_id = event->connection_id;
  response_queue_.back()->retransmit_timeout = kDefaultRetransmissionTimeout;
  response_queue_.back()->fabric_congestion_window =
      event->fabric_congestion_window;
  response_queue_.back()->nic_congestion_window = event->nic_congestion_window;
  response_queue_.back()->inter_packet_gap = 0;
  response_queue_.back()->nic_window_time_marker =
      event->nic_window_time_marker;
  response_queue_.back()->event_queue_select = event->event_queue_select;
  response_queue_.back()->delay_select = event->delay_select;
  response_queue_.back()->base_delay = event->base_delay;
  response_queue_.back()->delay_state = event->delay_state;
  response_queue_.back()->rtt_state = event->rtt_state;
  response_queue_.back()->cc_opaque = event->cc_opaque;
  response_queue_.back()->plb_state = event->plb_state;
  CHECK_OK_THEN_ASSIGN(
      ConnectionState* const connection_state,
      falcon_->get_state_manager()->PerformDirectLookup(event->connection_id));
  Gen2CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  int num_flows = ccmeta.gen2_flow_weights.size();
  response_queue_.back()->flow_id =
      GetFlowIdFromFlowLabel(event->flow_label, num_flows);
  response_queue_.back()->flow_label_1_weight = ccmeta.gen2_flow_weights[0];
  if (num_flows > 1) {
    response_queue_.back()->flow_label_2_weight = ccmeta.gen2_flow_weights[1];
  }
  if (num_flows > 2) {
    response_queue_.back()->flow_label_3_weight = ccmeta.gen2_flow_weights[2];
  }
  if (num_flows > 3) {
    response_queue_.back()->flow_label_4_weight = ccmeta.gen2_flow_weights[3];
  }
}

template <typename EventT, typename ResponseT>
int FalconTestingHelpers::FakeRueAdapter<EventT, ResponseT>::GetNumEvents()
    const {
  return event_queue_.size();
}

template <typename EventT, typename ResponseT>
int FalconTestingHelpers::FakeRueAdapter<EventT, ResponseT>::GetNumResponses()
    const {
  return response_queue_.size();
}

template <typename EventT, typename ResponseT>
void FalconTestingHelpers::FakeRueAdapter<EventT, ResponseT>::EnqueueAck(
    const Packet* packet, const CongestionControlMetadata& ccmeta,
    const RueKey* rue_key, uint32_t num_packets_acked, bool eack,
    bool eack_drop) {
  EventT event = {};
  format_adapter_->FillExplicitAckEvent(event, rue_key, packet, ccmeta,
                                        num_packets_acked, eack, eack_drop);
  event_queue_.push(std::make_unique<EventT>(event));
}

template <typename EventT, typename ResponseT>
void FalconTestingHelpers::FakeRueAdapter<EventT, ResponseT>::EnqueueNack(
    const RueKey* rue_key, const Packet* packet,
    const CongestionControlMetadata& ccmeta, uint32_t num_packets_acked) {
  EventT event = {};
  format_adapter_->FillNackEvent(event, rue_key, packet, ccmeta,
                                 num_packets_acked);
  event_queue_.push(std::make_unique<EventT>(event));
}

template <typename EventT, typename ResponseT>
void FalconTestingHelpers::FakeRueAdapter<EventT, ResponseT>::
    EnqueueTimeoutRetransmit(const RueKey* rue_key, const Packet* packet,
                             const CongestionControlMetadata& ccmeta,
                             uint8_t retransmit_count) {
  EventT event = {};
  format_adapter_->FillTimeoutRetransmittedEvent(event, rue_key, packet, ccmeta,
                                                 retransmit_count);
  event_queue_.push(std::make_unique<EventT>(event));
}

template <typename EventT, typename ResponseT>
ResponseMetadata
FalconTestingHelpers::FakeRueAdapter<EventT, ResponseT>::DequeueResponse(
    std::function<ConnectionState*(uint32_t connection_id)>
        connection_state_lookup) {
  auto response = std::move(response_queue_.front());
  response_queue_.pop();
  auto cid = response->connection_id;
  std::unique_ptr<RueKey> rue_key;
  if constexpr (std::is_same_v<ResponseT, falcon_rue::Response>) {
    rue_key = std::make_unique<RueKey>(cid);
  } else {
    auto flow_id = response->flow_id;
    rue_key = std::make_unique<Gen2RueKey>(cid, flow_id);
  }
  return ResponseMetadata{
      .randomize_path = format_adapter_->IsRandomizePath(response.get()),
      .has_rto_decreased = false,
      .rue_key = std::move(rue_key)};
}

template <typename EventT, typename ResponseT>
void FalconTestingHelpers::FakeRueAdapter<EventT, ResponseT>::
    InitializeMetadata(CongestionControlMetadata& metadata) const {}

template <typename EventT, typename ResponseT>
EventT FalconTestingHelpers::FakeRueAdapter<EventT, ResponseT>::FrontEvent()
    const {
  return *event_queue_.front();
}

template <typename EventT, typename ResponseT>
ResponseT
FalconTestingHelpers::FakeRueAdapter<EventT, ResponseT>::FrontResponse() const {
  return *response_queue_.front();
}

template <typename EventT, typename ResponseT>
void FalconTestingHelpers::FakeRueAdapter<EventT, ResponseT>::Set(
    std::unique_ptr<EventResponseFormatAdapter<EventT, ResponseT>>
        format_adapter) {
  format_adapter_ = std::move(format_adapter);
}

template <typename EventT, typename ResponseT>
void ProtocolRateUpdateEngineTestPeer<EventT, ResponseT>::Set(
    ProtocolRateUpdateEngine* rue) {
  rue_ = rue;
  auto rue_adapter = std::make_unique<
      FalconTestingHelpers::FakeRueAdapter<EventT, ResponseT>>();
  auto format_adapter =
      std::make_unique<EventResponseFormatAdapter<EventT, ResponseT>>(
          rue_->falcon_);
  rue_adapter->Set(std::move(format_adapter));
  rue_adapter->falcon_ = rue_->falcon_;
  rue_adapter_ = rue_adapter.get();
  rue_->rue_adapter_ = std::move(rue_adapter);
}

template <typename EventT, typename ResponseT>
uint32_t ProtocolRateUpdateEngineTestPeer<EventT, ResponseT>::ToFalconTimeUnits(
    absl::Duration time) const {
  return rue_->ToFalconTimeUnits(time);
}

template <typename EventT, typename ResponseT>
absl::Duration
ProtocolRateUpdateEngineTestPeer<EventT, ResponseT>::FromFalconTimeUnits(
    uint32_t time) const {
  return rue_->FromFalconTimeUnits(time);
}

template <typename EventT, typename ResponseT>
uint32_t
ProtocolRateUpdateEngineTestPeer<EventT, ResponseT>::ToTimingWheelTimeUnits(
    absl::Duration time) const {
  return rue_->ToTimingWheelTimeUnits(time);
}

template <typename EventT, typename ResponseT>
absl::Duration
ProtocolRateUpdateEngineTestPeer<EventT, ResponseT>::FromTimingWheelTimeUnits(
    uint32_t time) const {
  return rue_->FromTimingWheelTimeUnits(time);
}

template <typename EventT, typename ResponseT>
bool ProtocolRateUpdateEngineTestPeer<
    EventT, ResponseT>::IsEventQueueScheduled() const {
  return rue_->queue_scheduled_;
}

template <typename EventT, typename ResponseT>
int ProtocolRateUpdateEngineTestPeer<EventT, ResponseT>::GetNumEvents() const {
  return rue_adapter_->GetNumEvents();
}

template <typename EventT, typename ResponseT>
int ProtocolRateUpdateEngineTestPeer<EventT, ResponseT>::GetNumResponses()
    const {
  return rue_adapter_->GetNumResponses();
}

template <typename EventT, typename ResponseT>
EventT ProtocolRateUpdateEngineTestPeer<EventT, ResponseT>::FrontEvent() const {
  return rue_adapter_->FrontEvent();
}

template <typename EventT, typename ResponseT>
ResponseT ProtocolRateUpdateEngineTestPeer<EventT, ResponseT>::FrontResponse()
    const {
  return rue_adapter_->FrontResponse();
}

template <typename EventT, typename ResponseT>
void ProtocolRateUpdateEngineTestPeer<EventT, ResponseT>::IncrementNumAcked(
    const Packet* incoming_packet, uint32_t num_acked) {
  std::unique_ptr<RueKey> rue_key =
      rue_->GetRueKeyFromIncomingPacket(incoming_packet);
  CHECK_OK_THEN_ASSIGN(
      auto connection_state,
      rue_->falcon_->get_state_manager()->PerformDirectLookup(rue_key->scid));
  CongestionControlMetadata& ccmeta =
      *connection_state->congestion_control_metadata;
  if (rue_->falcon_->GetVersion() == 1) {
    ccmeta.num_acked += num_acked;
  } else if (rue_->falcon_->GetVersion() >= 2) {
    auto gen2_rue_key = dynamic_cast<const Gen2RueKey*>(rue_key.get());
    Gen2CongestionControlMetadata& gen2_ccmeta =
        CongestionControlMetadata::DowncastTo<Gen2CongestionControlMetadata>(
            ccmeta);
    gen2_ccmeta.gen2_num_acked[gen2_rue_key->flow_id] += num_acked;
  } else {
    // For ProtocolRateUpdateEngineTestPeer to be used for versions later than
    // 2, make sure to add the needed logic that accommodates that version's
    // changes.
    LOG(FATAL)
        << "Falcon version not supported in ProtocolRateUpdateEngineTestPeer";
  }
}

template <typename EventT, typename ResponseT>
std::unique_ptr<RueKey>
ProtocolRateUpdateEngineTestPeer<EventT, ResponseT>::GetRueKey(
    uint32_t cid, uint8_t flow_id) const {
  if (rue_->falcon_->GetVersion() == 1) {
    return std::make_unique<RueKey>(cid);
  } else if (rue_->falcon_->GetVersion() >= 2) {
    return std::make_unique<Gen2RueKey>(cid, flow_id);
  } else {
    // For ProtocolRateUpdateEngineTestPeer to be used for versions later than
    // 2, make sure to add the needed logic that accommodates that version's
    // changes.
    LOG(FATAL)
        << "Falcon version not supported in ProtocolRateUpdateEngineTestPeer";
  }
}

template <typename EventT, typename ResponseT>
bool ProtocolRateUpdateEngineTestPeer<
    EventT, ResponseT>::IsConnectionOutstanding(uint32_t cid,
                                                uint8_t flow_id) const {
  std::unique_ptr<RueKey> rue_key = GetRueKey(cid, flow_id);
  return rue_->GetOutstandingEvent(rue_key.get());
}

template <typename EventT, typename ResponseT>
uint32_t
ProtocolRateUpdateEngineTestPeer<EventT, ResponseT>::GetAccumulatedAcks(
    uint32_t cid, uint8_t flow_id) const {
  std::unique_ptr<RueKey> rue_key = GetRueKey(cid, flow_id);
  return rue_->GetNumAcked(rue_key.get());
}

template <typename EventT, typename ResponseT>
absl::Duration
ProtocolRateUpdateEngineTestPeer<EventT, ResponseT>::GetLastEventTime(
    uint32_t cid, uint8_t flow_id) const {
  std::unique_ptr<RueKey> rue_key = GetRueKey(cid, flow_id);
  return rue_->GetLastEventTime(rue_key.get());
}

template <typename EventT, typename ResponseT>
void ProtocolRateUpdateEngineTestPeer<
    EventT, ResponseT>::set_event_queue_thresholds(uint64_t threshold1,
                                                   uint64_t threshold2,
                                                   uint64_t threshold3) {
  rue_->event_queue_threshold_1_ = threshold1;
  rue_->event_queue_threshold_2_ = threshold2;
  rue_->event_queue_threshold_3_ = threshold3;
}

template <typename EventT, typename ResponseT>
void ProtocolRateUpdateEngineTestPeer<
    EventT, ResponseT>::set_predicate_1_time_threshold(absl::Duration t) {
  rue_->predicate_1_time_threshold_ = t;
}

template <typename EventT, typename ResponseT>
void ProtocolRateUpdateEngineTestPeer<
    EventT, ResponseT>::set_predicate_2_packet_count_threshold(uint32_t t) {
  rue_->predicate_2_packet_count_threshold_ = t;
}

// Explicit template instantiations for MockRueAdapter.
template class MockRueAdapter<falcon_rue::Event, falcon_rue::Response>;
template class MockRueAdapter<falcon_rue::Event_Gen2,
                              falcon_rue::Response_Gen2>;

// Explicit template instantiations for ProtocolRateUpdateEngineTestPeer.
template class ProtocolRateUpdateEngineTestPeer<falcon_rue::Event,
                                                falcon_rue::Response>;
template class ProtocolRateUpdateEngineTestPeer<falcon_rue::Event_Gen2,
                                                falcon_rue::Response_Gen2>;

}  // namespace isekai
