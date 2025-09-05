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

#include "isekai/common/net_address.h"

#include <arpa/inet.h>

#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "glog/logging.h"

namespace isekai {
namespace {

bool ParseByteInBase16(absl::string_view base16_string, uint8_t& byte) {
  if (base16_string.empty() || base16_string.size() > 2) {
    return false;
  }
  int buffer = 0;
  for (char c : base16_string) {
    if (!absl::ascii_isxdigit(c)) {
      return false;
    }
    int value =
        (c >= 'A') ? (c >= 'a') ? (c - 'a' + 10) : (c - 'A' + 10) : (c - '0');
    buffer = buffer * 16 + value;
  }
  memcpy(&byte, &buffer, 1);
  return true;
}

}  // namespace

absl::StatusOr<Ipv6Address> Ipv6Address::OfString(absl::string_view address) {
  struct in6_addr ipv6_address;
  if (inet_pton(AF_INET6, address.data(), &ipv6_address) == 1) {
    return Ipv6Address(ipv6_address);
  }
  return absl::InvalidArgumentError(
      absl::StrCat("Invalid input Ipv6 address: ", address));
}

std::string Ipv6Address::ToString() const {
  char addr_buf[INET6_ADDRSTRLEN];
  if (inet_ntop(AF_INET6, &ipv6_address_, addr_buf, INET6_ADDRSTRLEN) !=
      nullptr) {
    return std::string(std::move(addr_buf));
  }
  LOG(FATAL) << "Fail to convert ip address.";
}

absl::StatusOr<MacAddress> MacAddress::OfString(absl::string_view address) {
  std::vector<std::string> bytes = absl::StrSplit(address, ':');
  if (bytes.size() != 6) {
    return absl::InvalidArgumentError(
        absl::StrCat("Invalid input MAC address: ", address));
  }

  std::vector<uint8_t> address_buf(6);
  for (int i = 0; i < bytes.size(); ++i) {
    uint8_t byte;
    CHECK(ParseByteInBase16(bytes[i], byte));
    address_buf[i] = byte;
  }
  return MacAddress(address_buf[0], address_buf[1], address_buf[2],
                    address_buf[3], address_buf[4], address_buf[5]);
}

std::string MacAddress::ToString() const {
  uint8_t byte6 = (mac_address_ >> 40).to_ulong() & 0xFFu;
  uint8_t byte5 = (mac_address_ >> 32).to_ulong() & 0xFFu;
  uint8_t byte4 = (mac_address_ >> 24).to_ulong() & 0xFFu;
  uint8_t byte3 = (mac_address_ >> 16).to_ulong() & 0xFFu;
  uint8_t byte2 = (mac_address_ >> 8).to_ulong() & 0xFFu;
  uint8_t byte1 = (mac_address_ >> 0).to_ulong() & 0xFFu;
  return absl::StrFormat("%02x:%02x:%02x:%02x:%02x:%02x", byte6, byte5, byte4,
                         byte3, byte2, byte1);
}

}  // namespace isekai
