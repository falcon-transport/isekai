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

#ifndef ISEKAI_COMMON_COMMON_UTIL_H_
#define ISEKAI_COMMON_COMMON_UTIL_H_

#include <type_traits>

#include "absl/strings/string_view.h"
#include "isekai/common/config.pb.h"

namespace isekai {

template <typename To, typename From>
inline To down_cast(From* f) {
  static_assert((std::is_base_of<From, std::remove_pointer_t<To>>::value),
                "Error: target type is not derived from source type");
  return static_cast<To>(f);
}

constexpr size_t PspHeaderLength = 24;
constexpr size_t PspIntegrityChecksumValueLength = 16;

HostConfigProfile GetHostConfigProfile(absl::string_view host_id,
                                       const NetworkConfig& network);
}  // namespace isekai
#endif  // ISEKAI_COMMON_COMMON_UTIL_H_
