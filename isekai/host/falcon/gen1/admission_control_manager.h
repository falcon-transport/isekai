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

#ifndef ISEKAI_HOST_FALCON_FALCON_PROTOCOL_ADMISSION_CONTROL_MANAGER_H_
#define ISEKAI_HOST_FALCON_FALCON_PROTOCOL_ADMISSION_CONTROL_MANAGER_H_

#include <cstdint>
#include <memory>

#include "absl/status/status.h"
#include "isekai/common/token_bucket.h"
#include "isekai/host/falcon/falcon.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/falcon_connection_state.h"

namespace isekai {

// Holds the state related to performing window based admission control
struct WindowBasedAdmissionControlState {
  uint64_t admission_window_bytes;
  uint64_t solicited_data_bytes;
  explicit WindowBasedAdmissionControlState(uint64_t admission_window_bytes)
      : admission_window_bytes(admission_window_bytes),
        solicited_data_bytes(0) {}
};

class ProtocolAdmissionControlManager : public AdmissionControlManager {
 public:
  explicit ProtocolAdmissionControlManager(FalconModelInterface* falcon);
  // Checks to see if the packet in question meets the admission control
  // criteria.
  bool MeetsAdmissionControlCriteria(uint32_t scid, uint32_t rsn,
                                     falcon::PacketType type) override;
  // Checks if a packet of some request length and of some connection meets the
  // admission control criteria.
  bool MeetsAdmissionControlCriteria(
      uint64_t request_length, falcon::PacketType type,
      const ConnectionState* connection_state) override;
  // Reserves resources related to admission control. Reserved resources
  // correspond to bytes in the solicitation window in case of window-based
  // solicitation and tokens deducted in the token bucket in case of the rate
  // limiters.
  absl::Status ReserveAdmissionControlResource(
      uint32_t scid, uint32_t rsn, falcon::PacketType type) override;
  // Refunds the reserved resources once the transaction is complete, if
  // necessary. This method is meaningful only for window-based admission
  // control as it is closed-loop while the rate based ones are open loop and
  // hence do not need to refund.
  absl::Status RefundAdmissionControlResource(
      uint32_t scid, const TransactionKey& transaction_key) override;

 protected:
  FalconModelInterface* const falcon_;

 private:
  void InitRxWindowAdmissionControl();
  void InitRxRateLimiter();
  void InitTxRateLimiter();
  bool MeetsRxWindowBasedAdmissionCriteria(uint64_t request_length,
                                           falcon::PacketType type);
  bool MeetsRxRateBasedAdmissionCriteria(uint64_t request_length,
                                         falcon::PacketType type);
  bool MeetsTxRateBasedAdmissionCriteria(uint64_t request_length,
                                         falcon::PacketType type);
  void ReserveRxWindowBasedAdmissionResources(uint64_t request_length,
                                              falcon::PacketType type,
                                              uint32_t scid,
                                              TransactionMetadata* transaction);
  void ReserveRxRateBasedAdmissionResources(uint64_t request_length,
                                            falcon::PacketType type);
  void ReserveTxRateBasedAdmissionResources(uint64_t request_length,
                                            falcon::PacketType type);

  bool collect_solicitation_stats_ = false;

  std::unique_ptr<WindowBasedAdmissionControlState>
      window_admission_control_state_;
  std::unique_ptr<TokenBucket> rx_solicitation_rate_limiter_token_bucket_;
  std::unique_ptr<TokenBucket> tx_solicitation_rate_limiter_token_bucket_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_PROTOCOL_ADMISSION_CONTROL_MANAGER_H_
