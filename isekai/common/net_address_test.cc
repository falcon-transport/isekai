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

#include "absl/status/status.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/testing.h"

namespace {

TEST(Ipv6AddressTest, TestIpAddressConvertion) {
  const std::string ip_address = "::ffff:204.152.189.116";
  ASSERT_OK_AND_ASSIGN(auto binary_ip_address,
                       isekai::Ipv6Address::OfString(ip_address));
  EXPECT_EQ(binary_ip_address.ToString(), ip_address);

  auto address_convert_status = isekai::Ipv6Address::OfString("1.2.3").status();
  EXPECT_EQ(address_convert_status.code(), absl::StatusCode::kInvalidArgument);
}

TEST(MacAddressTest, TestMacAddressConvertion) {
  const std::string mac_address = "01:23:45:67:89:ab";
  ASSERT_OK_AND_ASSIGN(auto binary_mac_address,
                       isekai::MacAddress::OfString(mac_address));
  EXPECT_EQ(binary_mac_address.ToString(), mac_address);

  isekai::MacAddress mac_address2(0x01, 0x23, 0x45, 0x67, 0x89, 0xab);
  EXPECT_EQ(mac_address2.ToString(), mac_address);

  auto address_convert_status = isekai::MacAddress::OfString("1.2:3:").status();
  EXPECT_EQ(address_convert_status.code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
