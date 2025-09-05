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

#include "isekai/common/ipv6_trie.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <string>

#include "absl/numeric/int128.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/testing.h"

namespace {

constexpr uint8_t kTestIpv6AddressHex1[] = {0x20, 0x01, 0x0d, 0xb8, 0x85, 0xa3,
                                            0x00, 0x00, 0x00, 0x00, 0x8a, 0x2e,
                                            0x03, 0x70, 0x73, 0x34};
constexpr uint8_t kTestIpv6AddressHex2[] = {0x20, 0x01, 0x0d, 0xb8, 0x85, 0xa4,
                                            0x00, 0x00, 0x00, 0x00, 0x8a, 0x2e,
                                            0x03, 0x70, 0x73, 0x34};
constexpr uint8_t kTestIpv6AddressHex3[] = {0x20, 0x01, 0x0d, 0xb8, 0x85, 0xa3,
                                            0x00, 0x0a, 0x00, 0x00, 0x00, 0x00,
                                            0x00, 0x00, 0x00, 0x01};
constexpr uint8_t kTestIpv6AddressHex4[] = {0x20, 0x01, 0x0d, 0xb8, 0x85, 0xa4,
                                            0x00, 0x0a, 0x00, 0x00, 0x00, 0x00,
                                            0x00, 0x00, 0x00, 0x01};
constexpr uint8_t kTestIpv6Mask[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                     0xf0, 0x00, 0x00, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00};
constexpr char kTestIpv6Address1[] = "2001:db8:85a3::8a2e:370:7334";

TEST(Ipv6TrieTest, TestHexIpAddressConvert) {
  std::string address = isekai::HexIPv6AddressToString(
      std::string(kTestIpv6AddressHex1,
                  kTestIpv6AddressHex1 + sizeof(kTestIpv6AddressHex1)));
  EXPECT_EQ(address, kTestIpv6Address1);

  in6_addr binary_addr;
  inet_pton(AF_INET6, kTestIpv6Address1, &binary_addr);
  // Convert binary_addr1 (network byte order) to uint128 (host byte order).
  auto binary_addr_in_uint128 = absl::MakeUint128(
      static_cast<uint64_t>(ntohl(binary_addr.s6_addr32[0])) << 32 |
          static_cast<uint64_t>(ntohl(binary_addr.s6_addr32[1])),
      static_cast<uint64_t>(ntohl(binary_addr.s6_addr32[2])) << 32 |
          static_cast<uint64_t>(ntohl(binary_addr.s6_addr32[3])));
  absl::uint128 compare_binary_addr = isekai::HexIPv6AddressToBinaryFormat(
      std::string(kTestIpv6AddressHex1,
                  kTestIpv6AddressHex1 + sizeof(kTestIpv6AddressHex1)));
  EXPECT_EQ(binary_addr_in_uint128, compare_binary_addr);

  // Tests the convertion between string to ip address in uint128.
  auto addr_in_uint128_from_string =
      isekai::StringToIPAddressOrDie(kTestIpv6Address1);
  EXPECT_EQ(binary_addr_in_uint128, addr_in_uint128_from_string);
}

TEST(Ipv6TrieTest, TestTrie) {
  isekai::IPTable<uint32_t> trie;
  isekai::IPRange range1, range2;
  range1.address = isekai::HexIPv6AddressToBinaryFormat(
      std::string(kTestIpv6AddressHex1,
                  kTestIpv6AddressHex1 + sizeof(kTestIpv6AddressHex1)));
  range1.mask = isekai::HexIPv6AddressToBinaryFormat(
      std::string(kTestIpv6Mask, kTestIpv6Mask + sizeof(kTestIpv6Mask)));
  range2.address = isekai::HexIPv6AddressToBinaryFormat(
      std::string(kTestIpv6AddressHex2,
                  kTestIpv6AddressHex2 + sizeof(kTestIpv6AddressHex2)));
  range2.mask = isekai::HexIPv6AddressToBinaryFormat(
      std::string(kTestIpv6Mask, kTestIpv6Mask + sizeof(kTestIpv6Mask)));

  EXPECT_OK(trie.insert(std::make_pair(range1, 0), false));
  EXPECT_OK(trie.insert(std::make_pair(range2, 1), false));

  ASSERT_OK_AND_ASSIGN(
      const auto* a,
      trie.find_longest(isekai::HexIPv6AddressToBinaryFormat(
          std::string(kTestIpv6AddressHex3,
                      kTestIpv6AddressHex3 + sizeof(kTestIpv6AddressHex3)))));
  EXPECT_EQ(*a, 0);

  ASSERT_OK_AND_ASSIGN(
      const auto* b,
      trie.find_longest(isekai::HexIPv6AddressToBinaryFormat(
          std::string(kTestIpv6AddressHex4,
                      kTestIpv6AddressHex4 + sizeof(kTestIpv6AddressHex4)))));
  EXPECT_EQ(*b, 1);
}

}  // namespace
