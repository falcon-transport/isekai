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

#ifndef ISEKAI_HOST_FALCON_CONNECTION_STATE_UTILS_H_
#define ISEKAI_HOST_FALCON_CONNECTION_STATE_UTILS_H_

#include <cstdint>
#include <memory>

#include "isekai/host/falcon/gen1/falcon_types.h"
#include "isekai/host/falcon/gen2/falcon_types.h"

namespace isekai {

// Creates a transaction metadata object based on the falcon version.
inline std::unique_ptr<TransactionMetadata> CreateTransactionMetadata(
    uint32_t falcon_version, uint32_t rsn, TransactionType type,
    TransactionLocation location) {
  if (falcon_version == 1) {
    return std::make_unique<TransactionMetadata>(rsn, type, location);
  } else if (falcon_version == 2) {
    return std::make_unique<Gen2TransactionMetadata>(rsn, type, location);
  } else if (falcon_version > 2) {
    // No changes to transaction metadata for other versions > 2.
    return std::make_unique<Gen2TransactionMetadata>(rsn, type, location);
  } else {
    LOG(FATAL) << "Invalid Falcon protocol generation.";
  }
}

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_CONNECTION_STATE_UTILS_H_
