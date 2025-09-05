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

#ifndef ISEKAI_HOST_FALCON_GEN2_ULP_BACKPRESSURE_ALPHA_CARVING_H_
#define ISEKAI_HOST_FALCON_GEN2_ULP_BACKPRESSURE_ALPHA_CARVING_H_

#include <cstdint>

#include "isekai/common/model_interfaces.h"
#include "isekai/host/falcon/falcon_component_interfaces.h"
#include "isekai/host/falcon/gen2/falcon_component_interfaces.h"

namespace isekai {

class AlphaCarvingUlpBackpressurePolicy
    : public PerConnectionUlpBackpressurePolicy {
 public:
  AlphaCarvingUlpBackpressurePolicy(FalconModelInterface* falcon,
                                    uint32_t tx_packet_headroom);

  UlpXoff ComputeXoffStatus(uint32_t scid) override;

  FalconCredit ComputeAlphaCarvingForTesting(uint32_t scid) {
    return ComputeAlphaCarving(scid);
  }

 private:
  FalconModelInterface* const falcon_;
  const uint32_t tx_packet_headroom_;

  FalconCredit ComputeAlphaCarving(uint32_t scid);
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN2_ULP_BACKPRESSURE_ALPHA_CARVING_H_
