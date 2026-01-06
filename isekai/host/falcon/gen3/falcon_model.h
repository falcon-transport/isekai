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

#ifndef ISEKAI_HOST_FALCON_GEN3_FALCON_MODEL_H_
#define ISEKAI_HOST_FALCON_GEN3_FALCON_MODEL_H_

#include <cstdint>

#include "absl/strings/string_view.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/falcon_model.h"
#include "isekai/host/rnic/memory_interface.h"

namespace isekai {

class Gen3FalconModel : public Gen2FalconModel {
 public:
  explicit Gen3FalconModel(const FalconConfig& configuration, Environment* env,
                           StatisticCollectionInterface* stats_collector,
                           ConnectionManagerInterface* connection_manager,
                           std::string_view host_id, uint8_t number_of_hosts);
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_GEN3_FALCON_MODEL_H_
