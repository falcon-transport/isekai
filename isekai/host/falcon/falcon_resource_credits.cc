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

#include "isekai/host/falcon/falcon_resource_credits.h"

#include "isekai/common/config.pb.h"

namespace isekai {

FalconResourceCredits FalconResourceCredits::Create(
    const FalconConfig::ResourceCredits& config) {
  FalconResourceCredits credits = {
      .tx_packet_credits =
          {
              .ulp_requests = config.tx_packet_credits().ulp_requests(),
              .ulp_data = config.tx_packet_credits().ulp_data(),
              .network_requests = config.tx_packet_credits().network_requests(),
          },
      .tx_buffer_credits =
          {
              .ulp_requests = config.tx_buffer_credits().ulp_requests(),
              .ulp_data = config.tx_buffer_credits().ulp_data(),
              .network_requests = config.tx_buffer_credits().network_requests(),
          },
      .rx_packet_credits =
          {
              .ulp_requests = config.rx_packet_credits().ulp_requests(),
              .network_requests = config.rx_packet_credits().network_requests(),
          },
      .rx_buffer_credits =
          {
              .ulp_requests = config.rx_buffer_credits().ulp_requests(),
              .network_requests = config.rx_buffer_credits().network_requests(),
          },
  };

  return credits;
}

bool FalconResourceCredits::IsInitialized() const {
  return tx_packet_credits.ulp_requests != 0 ||
         tx_packet_credits.ulp_data != 0 ||
         tx_packet_credits.network_requests ||
         tx_buffer_credits.ulp_requests != 0 ||
         tx_buffer_credits.ulp_data != 0 ||
         tx_buffer_credits.network_requests != 0 ||
         rx_packet_credits.ulp_requests != 0 ||
         rx_packet_credits.network_requests != 0 ||
         rx_buffer_credits.ulp_requests != 0 ||
         rx_buffer_credits.network_requests != 0;
}

bool FalconResourceCredits::operator==(const FalconResourceCredits& rhs) const {
  return tx_packet_credits.ulp_requests == rhs.tx_packet_credits.ulp_requests &&
         tx_packet_credits.ulp_data == rhs.tx_packet_credits.ulp_data &&
         tx_packet_credits.network_requests ==
             rhs.tx_packet_credits.network_requests &&
         tx_buffer_credits.ulp_requests == rhs.tx_buffer_credits.ulp_requests &&
         tx_buffer_credits.ulp_data == rhs.tx_buffer_credits.ulp_data &&
         tx_buffer_credits.network_requests ==
             rhs.tx_buffer_credits.network_requests &&
         rx_packet_credits.ulp_requests == rhs.rx_packet_credits.ulp_requests &&
         rx_packet_credits.network_requests ==
             rhs.rx_packet_credits.network_requests &&
         rx_buffer_credits.ulp_requests == rhs.rx_buffer_credits.ulp_requests &&
         rx_buffer_credits.network_requests ==
             rhs.rx_buffer_credits.network_requests;
}

FalconResourceCredits& FalconResourceCredits::operator+=(
    const FalconResourceCredits& rhs) {
  // Add rhs for all resources except ulp_data.
  tx_packet_credits.ulp_requests += rhs.tx_packet_credits.ulp_requests;
  tx_packet_credits.network_requests += rhs.tx_packet_credits.network_requests;
  tx_buffer_credits.ulp_requests += rhs.tx_buffer_credits.ulp_requests;
  tx_buffer_credits.network_requests += rhs.tx_buffer_credits.network_requests;
  rx_packet_credits.ulp_requests += rhs.rx_packet_credits.ulp_requests;
  rx_packet_credits.network_requests += rhs.rx_packet_credits.network_requests;
  rx_buffer_credits.ulp_requests += rhs.rx_buffer_credits.ulp_requests;
  rx_buffer_credits.network_requests += rhs.rx_buffer_credits.network_requests;
  tx_packet_credits.ulp_data += rhs.tx_packet_credits.ulp_data;
  tx_buffer_credits.ulp_data += rhs.tx_buffer_credits.ulp_data;
  return *this;
}

FalconResourceCredits& FalconResourceCredits::operator-=(
    const FalconResourceCredits& rhs) {
  // Subtract rhs for all resources except ulp_data.
  tx_packet_credits.ulp_requests -= rhs.tx_packet_credits.ulp_requests;
  tx_packet_credits.network_requests -= rhs.tx_packet_credits.network_requests;
  tx_buffer_credits.ulp_requests -= rhs.tx_buffer_credits.ulp_requests;
  tx_buffer_credits.network_requests -= rhs.tx_buffer_credits.network_requests;
  rx_packet_credits.ulp_requests -= rhs.rx_packet_credits.ulp_requests;
  rx_packet_credits.network_requests -= rhs.rx_packet_credits.network_requests;
  rx_buffer_credits.ulp_requests -= rhs.rx_buffer_credits.ulp_requests;
  rx_buffer_credits.network_requests -= rhs.rx_buffer_credits.network_requests;
  tx_packet_credits.ulp_data -= rhs.tx_packet_credits.ulp_data;
  tx_buffer_credits.ulp_data -= rhs.tx_buffer_credits.ulp_data;
  return *this;
}

bool FalconResourceCredits::operator<=(const FalconResourceCredits& rhs) const {
  // Check whether all resources except ulp_data are within the limit of rhs.
  bool are_resources_within_limit =
      tx_packet_credits.ulp_requests <= rhs.tx_packet_credits.ulp_requests &&
      tx_packet_credits.network_requests <=
          rhs.tx_packet_credits.network_requests &&
      tx_buffer_credits.ulp_requests <= rhs.tx_buffer_credits.ulp_requests &&
      tx_buffer_credits.network_requests <=
          rhs.tx_buffer_credits.network_requests &&
      rx_packet_credits.ulp_requests <= rhs.rx_packet_credits.ulp_requests &&
      rx_packet_credits.network_requests <=
          rhs.rx_packet_credits.network_requests &&
      rx_buffer_credits.ulp_requests <= rhs.rx_buffer_credits.ulp_requests &&
      rx_buffer_credits.network_requests <=
          rhs.rx_buffer_credits.network_requests &&
      tx_packet_credits.ulp_data <= rhs.tx_packet_credits.ulp_data &&
      tx_buffer_credits.ulp_data <= rhs.tx_buffer_credits.ulp_data;
  return are_resources_within_limit;
}

}  // namespace isekai
