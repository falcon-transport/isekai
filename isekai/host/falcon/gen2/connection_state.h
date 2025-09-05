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

#ifndef ISEKAI_HOST_FALCON_GEN2_CONNECTION_STATE_H_
#define ISEKAI_HOST_FALCON_GEN2_CONNECTION_STATE_H_

#include <cstdint>
#include <memory>
#include <utility>

#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/falcon_types.h"

namespace isekai {

struct Gen2ConnectionState : ConnectionState {
  explicit Gen2ConnectionState(
      std::unique_ptr<ConnectionMetadata> connection_metadata_arg,
      std::unique_ptr<CongestionControlMetadata>
          congestion_control_metadata_arg,
      int version_arg = 2)
      : ConnectionState(std::move(connection_metadata_arg),
                        std::move(congestion_control_metadata_arg),
                        version_arg) {}
  virtual ~Gen2ConnectionState() {}

  // These values are used by the backpressure manager.
  ConnectionRdmaXoffMetadata connection_xoff_metadata;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN2_CONNECTION_STATE_H_
