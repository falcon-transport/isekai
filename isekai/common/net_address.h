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

#ifndef ISEKAI_OPEN_SOURCE_DEFAULT_NET_ADDRESS_H_
#define ISEKAI_OPEN_SOURCE_DEFAULT_NET_ADDRESS_H_

#include <netinet/in.h>

#include <bitset>
#include <cstdint>
#include <string>

#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace isekai {

class Ipv6Address {
 public:
  Ipv6Address() = default;
  static absl::StatusOr<Ipv6Address> OfString(absl::string_view address);
  std::string ToString() const;

  bool operator==(const Ipv6Address& other) const {
    if (ipv6_address_.s6_addr32[0] == other.ipv6_address_.s6_addr32[0] &&
        ipv6_address_.s6_addr32[1] == other.ipv6_address_.s6_addr32[1] &&
        ipv6_address_.s6_addr32[2] == other.ipv6_address_.s6_addr32[2] &&
        ipv6_address_.s6_addr32[3] == other.ipv6_address_.s6_addr32[3]) {
      return true;
    } else {
      return false;
    }
  }

  // Support hashing.
  template <typename H>
  friend H AbslHashValue(H h, const Ipv6Address& address) {
    return H::combine(std::move(h), address.ipv6_address_.s6_addr32[0],
                      address.ipv6_address_.s6_addr32[1],
                      address.ipv6_address_.s6_addr32[2],
                      address.ipv6_address_.s6_addr32[3]);
  }

 private:
  explicit Ipv6Address(const struct in6_addr& ipv6_address)
      : ipv6_address_(ipv6_address) {}

  struct in6_addr ipv6_address_;
};

class MacAddress {
 public:
  MacAddress() = default;
  explicit MacAddress(uint8_t byte6, uint8_t byte5, uint8_t byte4,
                      uint8_t byte3, uint8_t byte2, uint8_t byte1) {
    mac_address_ = (static_cast<uint64_t>(byte6) << 40) +
                   (static_cast<uint64_t>(byte5) << 32) +
                   (static_cast<uint64_t>(byte4) << 24) +
                   (static_cast<uint64_t>(byte3) << 16) +
                   (static_cast<uint64_t>(byte2) << 8) +
                   (static_cast<uint64_t>(byte1) << 0);
  }

  static absl::StatusOr<MacAddress> OfString(absl::string_view address);
  std::string ToString() const;

  bool operator==(const MacAddress& other) const {
    return mac_address_ == other.mac_address_;
  }

  // Support hashing.
  template <typename H>
  friend H AbslHashValue(H h, const MacAddress& address) {
    return H::combine(std::move(h), address.mac_address_);
  }

 private:
  std::bitset<48> mac_address_;
};

}  // namespace isekai

#endif  // ISEKAI_OPEN_SOURCE_DEFAULT_NET_ADDRESS_H_
