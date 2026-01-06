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

#ifndef ISEKAI_HOST_FALCON_GEN3_CONNECTION_STATE_H_
#define ISEKAI_HOST_FALCON_GEN3_CONNECTION_STATE_H_

#include <cstdint>
#include <memory>
#include <utility>

#include "absl/time/time.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/connection_state.h"

namespace isekai {

struct Gen3ConnectionState : Gen2ConnectionState {
  explicit Gen3ConnectionState(
      std::unique_ptr<ConnectionMetadata> connection_metadata_arg,
      std::unique_ptr<CongestionControlMetadata>
          congestion_control_metadata_arg,
      int version_arg = 3)
      : Gen2ConnectionState(std::move(connection_metadata_arg),
                            std::move(congestion_control_metadata_arg),
                            version_arg) {}
  virtual ~Gen3ConnectionState() {}

  // RACK states.
  struct Rack {
    // The most recent ACK's T1.
    absl::Duration xmit_ts = absl::ZeroDuration();
    // RACK RTO.
    absl::Duration rto = absl::InfiniteDuration();
    // Set upon receiving EACK, activates RACK for request (0) and/or data (1)
    // windows. Subsequent ACKs will disable RACK scans based on window BSPN.
    bool rack_enabled_for_window[2] = {false, false};
  } rack;

  // TLP states.
  struct Tlp {
    // Probe timeout.
    absl::Duration pto;
  } tlp;

  // Needed by RACK and/or TLP only. The greatest ACK BPSN received per window.
  uint32_t max_req_ack_bpsn = 0;
  uint32_t max_data_ack_bpsn = 0;

  // [For testing/debugging] Counter for number of packet scans by RACK.
  uint64_t rack_pkt_scan_count = 0;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN3_CONNECTION_STATE_H_
