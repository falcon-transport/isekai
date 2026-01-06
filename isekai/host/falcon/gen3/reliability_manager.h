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

#ifndef ISEKAI_HOST_FALCON_GEN3_RELIABILITY_MANAGER_H_
#define ISEKAI_HOST_FALCON_GEN3_RELIABILITY_MANAGER_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/time/time.h"
#include "isekai/common/packet.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/reliability_manager.h"

namespace isekai {

using RetransmissionPolicyAction =
    RetransmissionPolicy::RetransmissionPolicyAction;

class Gen3ReliabilityManager : public Gen2ReliabilityManager {
 public:
  explicit Gen3ReliabilityManager(FalconModelInterface* falcon);

  // Installs a provided retransmission policy by appending to retx_policies_.
  // Intended for testing as Gen3ReliabilityManager will install policies based
  // on FalconConfig.
  void InstallRetransmissionPolicyForTesting(
      std::unique_ptr<RetransmissionPolicy> policy);

  void InitializeConnection(uint32_t scid) override;

 private:
  absl::Status SetupRetransmission(ConnectionState* connection_state,
                                   TransactionMetadata* transaction,
                                   PacketMetadata* packet_metadata) override;
  absl::Status HandleACK(const Packet* packet) override;
  absl::Status HandleNack(const Packet* packet) override;
  void HandleImplicitAck(const Packet* packet,
                         ConnectionState* connection_state) override;
  void HandlePiggybackedACK(const Packet* packet,
                            ConnectionState* connection_state) override;
  void EnqueueAckToUlp(TransmitterReliabilityWindowMetadata* window,
                       uint32_t scid, uint32_t psn) override;
  void HandleRetransmitTimeout(uint32_t scid) override;
  // Post-process (update the related counters, etc.) the newly acked packet.
  void AfterPacketAcked(
      ConnectionState* connection_state,
      const OutstandingPacketContext* packet_context) override;
  // Chooses the flow label for a packet depending on its type.
  uint32_t ChooseOutgoingPacketFlowLabel(
      falcon::PacketType packet_type, uint32_t rsn,
      ConnectionState* connection_state) override;
  // Post-process (update the related counters, etc.) after flow label is
  // chosen.
  void AfterChooseOutgoingPacketFlowLabel(falcon::PacketType packet_type,
                                          uint32_t rsn,
                                          ConnectionState* connection_state,
                                          uint32_t old_flow_label,
                                          uint32_t new_flow_label) override;

  // Returns the minimum timeout between RTO, RNR, etc (gen2 ReliabilityManager)
  // and any RetransmissionPolicy timeouts specified.
  absl::Duration GetTimeoutOfPacket(ConnectionState* connection_state,
                                    PacketMetadata* packet_metadata) override;

  // Update the outstanding counter corresponding to the packet_context.
  void UpdateOutstandingCounter(ConnectionState* connection_state,
                                uint8_t flow_id, int delta);

  // Update the outstanding counter after flow label Changes
  void UpdateOutstandingCounterAfterNewFlowLabel(
      ConnectionState* connection_state, uint8_t old_flow_id,
      uint8_t new_flow_id, bool is_retx);

  // For each retransmission policy in retx_policies_, invokes the specified
  // handler function for an event. The policy_handler_fn will be called with
  // each RetransmissionPolicy*, and should be defined such as:
  //   [connection_state](RetransmissionPolicy* policy) {
  //     return policy->...(connection_state, ...);
  //   }
  // This policy_handler_fn specification allows the caller to specify one
  // method to be invoked on each RetransmissionPolicy instance without
  // repeating the logic to iterate over policies and collect returned actions.
  // Returned actions are performed using ExecuteAllRetransmissionPolicyActions,
  // which coalesces duplicate retransmission work IDs. Individual actions
  // (retransmissions and scheduling events) are then performed by
  // ExecuteRetransmissionPolicyActions.
  void InvokeRetransmissionPolicies(
      ConnectionState* connection_state,
      absl::AnyInvocable<RetransmissionPolicyAction(RetransmissionPolicy*)>
          policy_handler_fn);

  // Executes action returned by provided retransmission policy, by performing
  // retransmissions and scheduling future events.
  void ExecuteRetransmissionPolicyAction(
      ConnectionState* connection_state, RetransmissionPolicy* policy,
      const RetransmissionPolicyAction& action);

  // Executes unique actions returned by the retransmission policies in
  // retx_policy_actions_ after handling an event. Duplicate
  // RetransmissionWorkIds arising from multiple policies will be handled
  // only once.
  void ExecuteAllRetransmissionPolicyActions(ConnectionState* connection_state);

  // Active retransmission policies enabled within FalconConfig.
  std::vector<std::unique_ptr<RetransmissionPolicy>> retx_policies_;

  // Actions requested by each retransmission policy after handling an event.
  // Each element corresponds to policy at same index in retx_policy_.
  std::vector<RetransmissionPolicyAction> retx_policy_actions_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN3_RELIABILITY_MANAGER_H_
