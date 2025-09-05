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

#ifndef ISEKAI_HOST_FALCON_FALCON_RESOURCE_CREDITS_H_
#define ISEKAI_HOST_FALCON_FALCON_RESOURCE_CREDITS_H_

#include <cstdint>

#include "isekai/common/config.pb.h"

namespace isekai {

struct FalconResourceCredits {
  static FalconResourceCredits Create(
      const FalconConfig::ResourceCredits& config);

  struct TxPacketCredits {
    int32_t ulp_requests = 0;
    int32_t ulp_data = 0;
    int32_t network_requests = 0;
  } tx_packet_credits;
  struct TxBufferCredits {
    int32_t ulp_requests = 0;
    int32_t ulp_data = 0;
    int32_t network_requests = 0;
  } tx_buffer_credits;
  struct RxPacketCredits {
    int32_t ulp_requests = 0;
    int32_t network_requests = 0;
  } rx_packet_credits;
  struct RxBufferCredits {
    int32_t ulp_requests = 0;
    int32_t network_requests = 0;
  } rx_buffer_credits;

  bool IsInitialized() const;
  bool operator<=(const FalconResourceCredits& rhs) const;
  bool operator==(const FalconResourceCredits& rhs) const;
  FalconResourceCredits& operator+=(const FalconResourceCredits& rhs);
  FalconResourceCredits& operator-=(const FalconResourceCredits& rhs);
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_RESOURCE_CREDITS_H_
