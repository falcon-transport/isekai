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

#include "isekai/host/falcon/gen3/reliability_manager.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "glog/logging.h"
#include "isekai/common/packet.h"
#include "isekai/common/status_util.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/connection_scheduler_types.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen1/falcon_utils.h"
#include "isekai/host/falcon/gen1/packet_reliability_manager.h"
#include "isekai/host/falcon/gen2/falcon_types.h"
#include "isekai/host/falcon/gen2/falcon_utils.h"
#include "isekai/host/falcon/gen2/reliability_manager.h"
#include "isekai/host/falcon/gen3/falcon_types.h"
#include "isekai/host/falcon/gen3/rack_retransmission_policy.h"
#include "isekai/host/falcon/gen3/strict_priority_policy.h"
#include "isekai/host/falcon/gen3/tlp_retransmission_policy.h"
#include "isekai/host/falcon/rue/bits.h"
#include "isekai/host/falcon/rue/fixed.h"

namespace isekai {

Gen3ReliabilityManager::Gen3ReliabilityManager(FalconModelInterface* falcon)
    : Gen2ReliabilityManager(falcon) {
  const FalconConfig* config = falcon->get_config();
  if (config->early_retx().enable_rack()) {
    retx_policies_.push_back(
        std::make_unique<RackRetransmissionPolicy>(falcon));
  }
  if (config->early_retx().enable_tlp()) {
    retx_policies_.push_back(std::make_unique<TlpRetransmissionPolicy>(falcon));
  }

  retx_policy_actions_.reserve(retx_policies_.size());
}

void Gen3ReliabilityManager::InstallRetransmissionPolicyForTesting(
    std::unique_ptr<RetransmissionPolicy> policy) {
  retx_policies_.push_back(std::move(policy));
  retx_policy_actions_.reserve(retx_policies_.size());
}

void Gen3ReliabilityManager::InitializeConnection(uint32_t scid) {
  CHECK_OK_THEN_ASSIGN(ConnectionState * connection_state,
                       falcon_->get_state_manager()->PerformDirectLookup(scid));

  InvokeRetransmissionPolicies(
      connection_state, [connection_state](RetransmissionPolicy* policy) {
        return policy->InitConnection(connection_state);
      });

  if (multipath_config_.path_selection_policy ==
          FalconConfig::Gen2ConfigOptions::MultipathConfig::
              OPEN_LOOP_ROUND_ROBIN ||
      multipath_config_.path_selection_policy ==
          FalconConfig::Gen2ConfigOptions::MultipathConfig::
              OPEN_LOOP_FCWND_WEIGHTED_ROUND_ROBIN) {
    return Gen2ReliabilityManager::InitializeConnection(scid);
  }

  if (multipath_config_.path_selection_policy !=
      FalconConfig::Gen2ConfigOptions::MultipathConfig::
          CLOSED_LOOP_MAX_OPEN_FCWND_RR_WHEN_TIE) {
    LOG(FATAL) << "Unsupported path selection policy: "
               << multipath_config_.path_selection_policy;
  }

  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  // Larger open fcwnd means higher priority.
  StrictPriorityPolicy<uint8_t>::PriorityFetcher fetch_flow_priority =
      [&ccmeta](uint8_t flow_id) {
        int flow_fcwnd =
            static_cast<int>(falcon_rue::FixedToUint<uint32_t, uint32_t>(
                ccmeta.gen3_flow_fcwnds[flow_id], falcon_rue::kFractionalBits));
        return (flow_fcwnd - ccmeta.gen3_outstanding_count[flow_id]);
      };
  auto sp_policy =
      std::make_unique<StrictPriorityPolicy<uint8_t>>(fetch_flow_priority);
  uint32_t num_flows = ccmeta.gen2_flow_labels.size();
  for (uint8_t flow_id = 0; flow_id < num_flows; ++flow_id) {
    CHECK_OK(sp_policy->InitializeEntity(flow_id));
  }
  path_selection_policies_[scid] = std::move(sp_policy);
}

absl::Status Gen3ReliabilityManager::SetupRetransmission(
    ConnectionState* connection_state, TransactionMetadata* transaction,
    PacketMetadata* packet_metadata) {
  InvokeRetransmissionPolicies(
      connection_state,
      [packet_metadata, connection_state](RetransmissionPolicy* policy) {
        return policy->SetupRetransmission(packet_metadata, connection_state);
      });

  return Gen2ReliabilityManager::SetupRetransmission(
      connection_state, transaction, packet_metadata);
}

absl::Status Gen3ReliabilityManager::HandleACK(const Packet* packet) {
  CHECK_OK_THEN_ASSIGN(
      ConnectionState * connection_state,
      falcon_->get_state_manager()->PerformDirectLookup(packet->ack.dest_cid));

  InvokeRetransmissionPolicies(
      connection_state,
      [packet, connection_state](RetransmissionPolicy* policy) {
        return policy->HandleAck(packet, connection_state);
      });

  return Gen2ReliabilityManager::HandleACK(packet);
}

absl::Status Gen3ReliabilityManager::HandleNack(const Packet* packet) {
  CHECK_OK_THEN_ASSIGN(
      ConnectionState * connection_state,
      falcon_->get_state_manager()->PerformDirectLookup(packet->nack.dest_cid));

  InvokeRetransmissionPolicies(
      connection_state,
      [packet, connection_state](RetransmissionPolicy* policy) {
        return policy->HandleNack(packet, connection_state);
      });

  return Gen2ReliabilityManager::HandleNack(packet);
}

void Gen3ReliabilityManager::HandleImplicitAck(
    const Packet* packet, ConnectionState* connection_state) {
  InvokeRetransmissionPolicies(
      connection_state,
      [packet, connection_state](RetransmissionPolicy* policy) {
        return policy->HandleImplicitAck(packet, connection_state);
      });

  Gen2ReliabilityManager::HandleImplicitAck(packet, connection_state);
}

void Gen3ReliabilityManager::HandlePiggybackedACK(
    const Packet* packet, ConnectionState* connection_state) {
  InvokeRetransmissionPolicies(
      connection_state,
      [packet, connection_state](RetransmissionPolicy* policy) {
        return policy->HandlePiggybackedAck(packet, connection_state);
      });

  Gen2ReliabilityManager::HandlePiggybackedACK(packet, connection_state);
}

void Gen3ReliabilityManager::EnqueueAckToUlp(
    TransmitterReliabilityWindowMetadata* window, uint32_t scid, uint32_t psn) {
  // Look up the associated PacketMetadata to provide to RetransmissionPolicy
  // handlers.
  CHECK_OK_THEN_ASSIGN(ConnectionState * connection_state,
                       falcon_->get_state_manager()->PerformDirectLookup(scid));
  CHECK_OK_THEN_ASSIGN(const OutstandingPacketContext* packet_context,
                       window->GetOutstandingPacketRSN(psn));
  auto location =
      GetTransactionLocation(packet_context->packet_type, /*incoming=*/false);
  CHECK_OK_THEN_ASSIGN(
      TransactionMetadata * transaction,
      connection_state->GetTransaction({packet_context->rsn, location}));
  CHECK_OK_THEN_ASSIGN(
      PacketMetadata * packet_metadata,
      transaction->GetPacketMetadata(packet_context->packet_type));

  InvokeRetransmissionPolicies(
      connection_state,
      [packet_metadata, connection_state](RetransmissionPolicy* policy) {
        return policy->HandleAckToUlp(packet_metadata, connection_state);
      });

  Gen2ReliabilityManager::EnqueueAckToUlp(window, scid, psn);
}

// Handles timeout event by first invoking early retransmission policy handlers,
// and performing any resulting actions. Then, performs base RTO scan using the
// Gen2ReliabilityManager implementation. Early retransmission packets will
// therefore be transmitted first, in the order returned by each retransmission
// policy, followed by any remaining packets meeting the RTO criteria.
void Gen3ReliabilityManager::HandleRetransmitTimeout(uint32_t scid) {
  CHECK_OK_THEN_ASSIGN(ConnectionState * connection_state,
                       falcon_->get_state_manager()->PerformDirectLookup(scid));

  //
  // avoid repeating ElapsedTime check.
  // If this is an invalid timeout, return.
  if (!falcon_->get_environment()->ElapsedTimeEquals(
          connection_state->tx_reliability_metadata.GetRtoExpirationTime())) {
    return;
  }

  // First perform any early retransmission actions.
  InvokeRetransmissionPolicies(
      connection_state, [connection_state](RetransmissionPolicy* policy) {
        return policy->HandleRetransmitTimeoutEvent(connection_state);
      });

  // Then perform base RTO scan. This will skip retransmission for any packets
  // already handled by an RetransmissionPolicy actions above, since
  // ExecuteRetransmissionPolicyAction removes them from the outstanding_packets
  // set upon retx, and this set is scanned by Gen2 HandleRetransmitTimeout for
  // base RTO criteria.
  Gen2ReliabilityManager::HandleRetransmitTimeout(scid);
}

// Post-process (update the related counters, etc.) the newly acked packet.
void Gen3ReliabilityManager::AfterPacketAcked(
    ConnectionState* connection_state,
    const OutstandingPacketContext* packet_context) {
  auto gen2_packet_context =
      dynamic_cast<const Gen2OutstandingPacketContext*>(packet_context);
  uint8_t flow_id = gen2_packet_context->flow_id;
  UpdateOutstandingCounter(connection_state, flow_id, -1);
  Gen2ReliabilityManager::AfterPacketAcked(connection_state, packet_context);
}

// Update the outstanding counter for the given packet context.
void Gen3ReliabilityManager::UpdateOutstandingCounter(
    ConnectionState* connection_state, uint8_t flow_id, int delta) {
  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  ccmeta.gen3_outstanding_count[flow_id] += delta;
}

// For initial retransmissions, uses WRR/RR policy to choose the flow label. For
// retransmissions, uses the same flow ID as original initial transmission; the
// flow label can change from the initial transmission if RUE changed that
// flow's label since the initial transmission.
uint32_t Gen3ReliabilityManager::ChooseOutgoingPacketFlowLabel(
    falcon::PacketType packet_type, uint32_t rsn,
    ConnectionState* connection_state) {
  // Get a handle on the packet's PacketMetadata.
  CHECK_OK_THEN_ASSIGN(TransactionMetadata* const transaction,
                       connection_state->GetTransaction(
                           TransactionKey(rsn, GetTransactionLocation(
                                                   /*type=*/packet_type,
                                                   /*incoming=*/false))));
  CHECK_OK_THEN_ASSIGN(PacketMetadata* const packet_metadata,
                       transaction->GetPacketMetadata(packet_type));

  if (packet_metadata->transmit_attempts > 1) {
    // The outgoing packet is a retransmission. Use same flow ID as initial
    // transmission.
    const Packet* packet = packet_metadata->active_packet.get();
    //
    // retransmission.
    return Gen2ReliabilityManager::ChooseRetxFlowLabel(packet,
                                                       connection_state);
  }
  // The outgoing packet is an initial transmission. Use WRR/RR policy to
  // choose the flow label.
  uint32_t scid = connection_state->connection_metadata->scid;
  CHECK_OK_THEN_ASSIGN(uint8_t chosen_flow_id,
                       path_selection_policies_[scid]->GetNextEntity());
  // Return the up-to-date flow label for the chosen flow ID.
  Gen3CongestionControlMetadata& ccmeta =
      CongestionControlMetadata::DowncastTo<Gen3CongestionControlMetadata>(
          *connection_state->congestion_control_metadata);
  return ccmeta.gen2_flow_labels[chosen_flow_id];
}

// Post-process (update the related counters, etc.) after flow label is
// chosen.
void Gen3ReliabilityManager::AfterChooseOutgoingPacketFlowLabel(
    falcon::PacketType packet_type, uint32_t rsn,
    ConnectionState* connection_state, uint32_t old_flow_label,
    uint32_t new_flow_label) {
  // Update outstanding count for old_flow and new_flow.
  CHECK_OK_THEN_ASSIGN(TransactionMetadata* const transaction,
                       connection_state->GetTransaction(
                           TransactionKey(rsn, GetTransactionLocation(
                                                   /*type=*/packet_type,
                                                   /*incoming=*/false))));
  CHECK_OK_THEN_ASSIGN(PacketMetadata* const packet_metadata,
                       transaction->GetPacketMetadata(packet_type));
  uint32_t scid = connection_state->connection_metadata->scid;
  uint8_t old_flow_id = GetFlowIdFromFlowLabel(old_flow_label, falcon_, scid);
  uint8_t new_flow_id = GetFlowIdFromFlowLabel(new_flow_label, falcon_, scid);
  UpdateOutstandingCounterAfterNewFlowLabel(
      connection_state, old_flow_id, new_flow_id,
      packet_metadata->transmit_attempts > 1);
  Gen2ReliabilityManager::AfterChooseOutgoingPacketFlowLabel(
      packet_type, rsn, connection_state, old_flow_label, new_flow_label);
}

// Update the outstanding counter after flow label changes.
void Gen3ReliabilityManager::UpdateOutstandingCounterAfterNewFlowLabel(
    ConnectionState* connection_state, uint8_t old_flow_id, uint8_t new_flow_id,
    bool is_retx) {
  if (is_retx) {
    UpdateOutstandingCounter(connection_state, old_flow_id, -1);
  }
  UpdateOutstandingCounter(connection_state, new_flow_id, 1);
}

// Invokes the action specified by policy_handler_fn for each retransmission
// policy in retx_policies_. The policy_handler_fn will be called with each
// RetransmissionPolicy*, and should be defined such as:
//   [connection_state](RetransmissionPolicy* policy) {
//     return policy->InitConnection(connection_state);
//   }
// Returned actions will be performed via ExecuteRetransmissionPolicyActions
// after coalescing any duplicate actions.
void Gen3ReliabilityManager::InvokeRetransmissionPolicies(
    ConnectionState* connection_state,
    absl::AnyInvocable<RetransmissionPolicyAction(RetransmissionPolicy* policy)>
        policy_handler_fn) {
  retx_policy_actions_.clear();
  for (auto& policy : retx_policies_) {
    retx_policy_actions_.emplace_back(policy_handler_fn(policy.get()));
  }
  ExecuteAllRetransmissionPolicyActions(connection_state);
}

// Handles the actions returned by a retransmission policy, which include
// packet retransmissions and scheduling future events for the policy at a
// specified duration.
void Gen3ReliabilityManager::ExecuteRetransmissionPolicyAction(
    ConnectionState* connection_state, RetransmissionPolicy* policy,
    const RetransmissionPolicyAction& action) {
  // Schedule a future event if a nonzero duration is provided.
  if (action.scheduled_event_duration != absl::ZeroDuration()) {
    auto status = falcon_->get_environment()->ScheduleEvent(
        action.scheduled_event_duration, [this, policy, connection_state]() {
          const auto& scheduled_event_action =
              policy->HandleScheduledEvent(connection_state);
          this->ExecuteRetransmissionPolicyAction(connection_state, policy,
                                                  scheduled_event_action);
        });
    if (!status.ok()) {
      LOG(FATAL) << "Failed to schedule RetransmissionPolicy event: " << status;
    }
  }

  // Handle any immediate retransmissions.
  for (const RetransmissionWorkId& work_id : action.retx_work_ids) {
    InitiateRetransmission(connection_state->connection_metadata->scid,
                           work_id.rsn, work_id.type);
    auto* tx_window = GetAppropriateTxWindow(
        &connection_state->tx_reliability_metadata, work_id.type);
    tx_window->outstanding_packets.erase(work_id);
  }
}

// Executes all actions returned by installed retransmission policies (in
// retx_policy_actions_). Duplicate RetransmissionWorkIds arising from multiple
// policies will be handled only once, and multiple retransmission policy events
// scheduled for the same duration will be coalesced into a single simulation
// event.
//
// skip duplicate RetransmissionWorkIds.
void Gen3ReliabilityManager::ExecuteAllRetransmissionPolicyActions(
    ConnectionState* connection_state) {
  // Elements in action_list should map 1:1 to retx_policies_.
  DCHECK_EQ(retx_policies_.size(), retx_policy_actions_.size());
  for (int i = 0; i < retx_policies_.size(); ++i) {
    ExecuteRetransmissionPolicyAction(connection_state, retx_policies_[i].get(),
                                      retx_policy_actions_[i]);
  }
}

// Returns the minimum retransmission timeout for a given packet between the
// base RTO calculation (provided by Gen2ReliabilityManager) and any Gen3
// early retransmission policies installed in retx_policies_.
absl::Duration Gen3ReliabilityManager::GetTimeoutOfPacket(
    ConnectionState* connection_state, PacketMetadata* packet_metadata) {
  absl::Duration base_timeout = Gen2ReliabilityManager::GetTimeoutOfPacket(
      connection_state, packet_metadata);

  for (auto& policy : retx_policies_) {
    absl::Duration retx_policy_timeout =
        policy->GetTimeoutOfPacket(connection_state, packet_metadata);
    if (retx_policy_timeout < base_timeout) {
      base_timeout = retx_policy_timeout;
    }
  }
  return base_timeout;
}

}  // namespace isekai
