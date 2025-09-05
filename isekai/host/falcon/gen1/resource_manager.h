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

#ifndef ISEKAI_HOST_FALCON_FALCON_PROTOCOL_RESOURCE_MANAGER_H_
#define ISEKAI_HOST_FALCON_FALCON_PROTOCOL_RESOURCE_MANAGER_H_

#include <array>
#include <cstdint>

#include "absl/status/status.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_resource_credits.h"

namespace isekai {

enum class RxNetworkRequestZone {
  kGreen = 0,   // Allow any incoming network request
  kYellow = 1,  // Allow any incoming HoL network request
  kRed = 2,     // Allow only HoL pull request or push unsolicited data
};

struct NetworkRegionEmaOccupancy {
  uint32_t rx_buffer_pool_occupancy_ema = 0;
  uint32_t rx_packet_pool_occupancy_ema = 0;
  uint32_t tx_packet_pool_occupancy_ema = 0;
};

// Manages Falcon block resources pools across connections. The credit
// calculation is based on the resource calculation equations mentioned in the
// Gen_1 MAS - HAS 2.2.
class ProtocolResourceManager : public ResourceManager {
 public:
  static constexpr uint32_t kEmaOccupancyFractionalBits = 15;
  static constexpr uint32_t kTargetBufferOccupancyBits = 5;

  explicit ProtocolResourceManager(FalconModelInterface* falcon);

  // Reserves the necessary TX/RX resources for the transaction or checks
  // availability, if required.
  absl::Status VerifyResourceAvailabilityOrReserveResources(
      uint32_t scid, const Packet* packet, PacketDirection direction,
      bool reserve_resources) override;
  // Releases the necessary TX/RX resources for the transaction, if required.
  absl::Status ReleaseResources(uint32_t scid,
                                const TransactionKey& transaction_key,
                                falcon::PacketType type) override;
  // Returns the 5-bit quantized value of the most occupied resource of the
  // network region resource pool.
  uint16_t GetNetworkRegionOccupancy() const override {
    return network_region_occupancy_;
  }

  FalconResourceCredits& GetAvailableResourceCreditsForTesting() {
    return global_credits_;
  }

  FalconResourceCredits GetAvailableResourceCredits() const {
    return global_credits_;
  }

  bool GetRequestXoffForTesting() { return request_xoff_; }
  bool GetGlobalXoffForTesting() { return global_xoff_; }

  NetworkRegionEmaOccupancy UpdateNetworkRegionEmaOccupancyForTesting();

 protected:
  FalconModelInterface* const falcon_;
  // Returns credits corresponding to RDMA managed resources to RDMA, if
  // required.
  virtual void ReturnRdmaManagedFalconResourceCredits(
      ConnectionState* connection_state, const TransactionKey& transaction_key,
      FalconCredit rdma_managed_resource_credits);

 private:
  // Following methods return the credits required to carry out a given
  // transaction based on its direction. data_length includes CRTBTH, RDMA
  // headers and inline data. sgl_length includes length of SGL. payload_length
  // represents the length of FALCON payload (used typically for handling
  // network requests). request_length represents the expected response/request
  // length of future transactions.
  FalconResourceCredits ComputeOutgoingPullRequestResources(
      uint32_t data_length, uint32_t sgl_length);
  FalconResourceCredits ComputeIncomingPullRequestResources(
      uint32_t payload_length);

  FalconResourceCredits ComputeOutgoingPullDataResources(uint32_t data_length,
                                                         uint32_t sgl_length);
  FalconResourceCredits ComputeIncomingPullDataResources(
      uint32_t request_length);

  FalconResourceCredits ComputeOutgoingPushRequestResources(
      uint32_t data_length, uint32_t sgl_length);
  FalconResourceCredits ComputeIncomingPushRequestResources();

  FalconResourceCredits ComputeOutgoingPushGrantResources();
  FalconResourceCredits ComputeIncomingPushGrantResources();

  FalconResourceCredits ComputeOutgoingPushSolicitedDataResources(
      uint32_t data_length, uint32_t sgl_length);
  FalconResourceCredits ComputeIncomingPushSolicitedDataResources(
      uint32_t request_length);

  FalconResourceCredits ComputeOutgoingPushUnsolicitedDataResources(
      uint32_t data_length, uint32_t sgl_length);
  FalconResourceCredits ComputeIncomingPushUnsolicitedDataResources(
      uint32_t request_length);
  FalconResourceCredits ComputeIncomingPushUnsolicitedCompletionResources();

  // Checks request resources and asserts Xoff to ULP if any of the 4 have
  // fallen below the configured threshold.
  void CheckUlpRequestXoff();

  // Checks whether the incoming request meets the admission criteria or not.
  bool MeetsNetworkRequestAdmissionCriteria(uint32_t cid, uint32_t rsn,
                                            falcon::PacketType type);

  // Returns the occupancy zone of the network requests RX buffer pool.
  RxNetworkRequestZone GetNetworkRequestRxBufferPoolOccupancyZone();
  // Returns the occupancy zone of the network requests RX packet pool.
  RxNetworkRequestZone GetNetworkRequestRxPacketPoolOccupancyZone();
  // Returns the occupancy zone of the network requests TX packet pool.
  RxNetworkRequestZone GetNetworkRequestTxPacketPoolOccupancyZone();

  // Updates the EMA values of the network region buffer occupancy.
  void UpdateNetworkRegionEmaOccupancy();

  // Updates the qantized network region occupancy to the max(rx_pkt, rx_buf,
  // tx_pkt).
  void UpdateQuantizedNetworkRegionOccupancy();

  // Updates the resources availability to the stats collection framework.
  void UpdateResourceAvailabilityCounters();

  // Represent global credits corresponding to FALCON resource pools.
  FalconResourceCredits global_credits_;

  // Represents the limits below which request/global xoff is asserted to ULP.
  const FalconResourceCredits request_xoff_limit_;
  const FalconResourceCredits global_xoff_limit_;

  // Current request or global Xoff status that has been asserted to ULP.
  bool request_xoff_ = false;
  bool global_xoff_ = false;

  NetworkRegionEmaOccupancy network_region_ema_;

  // Holds the quantized value of the maximum occupied resource.
  uint16_t network_region_occupancy_ : kTargetBufferOccupancyBits;

  bool collect_ema_occupancy_ = false;
  bool collect_resource_credit_timeline_ = false;
};

};  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_PROTOCOL_RESOURCE_MANAGER_H_
