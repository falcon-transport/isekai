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

#ifndef ISEKAI_OPEN_SOURCE_DEFAULT_IPV6_TRIE_H_
#define ISEKAI_OPEN_SOURCE_DEFAULT_IPV6_TRIE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "absl/numeric/int128.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"

// We currently only support IPv6 lookup.

namespace isekai {

struct IPRange {
  absl::uint128 address;
  absl::uint128 mask;

  // Support hashing.
  template <typename H>
  friend H AbslHashValue(H h, const IPRange& s) {
    return H::combine(std::move(h), s.address, s.mask);
  }

  const inline bool operator==(const IPRange& s) const {
    return (address == s.address && mask == s.mask);
  }

  IPRange(absl::uint128 address, absl::uint128 mask)
      : address(address), mask(mask) {}
  IPRange() {}
};

// Converts the ipv6 address into byte order.
absl::uint128 HexIPv6AddressToBinaryFormat(const std::string& hex_ipv6_address);

std::string HexIPv6AddressToString(const std::string& hex_ipv6_address);

absl::uint128 StringToIPAddressOrDie(const std::string& str);

template <class T>
struct TrieNode {
  std::unique_ptr<TrieNode> left;
  std::unique_ptr<TrieNode> right;
  std::optional<T> data = std::nullopt;
};

template <class T>
class IPTableInterface {
 public:
  virtual ~IPTableInterface() {}
  virtual absl::Status insert(std::pair<IPRange, T> value, bool overwrite) = 0;
  virtual absl::StatusOr<const T*> find_longest(
      const absl::uint128 address) const = 0;
};

template <class T>
class IPTable : public IPTableInterface<T> {
 public:
  absl::Status insert(std::pair<IPRange, T> value,
                      bool overwrite = true) override {
    const uint64_t high_bit = 0x8000000000000000;
    uint64_t bit = high_bit;
    uint32_t count = 0;
    // Starting from high 64 bits.
    uint64_t addr = absl::Uint128High64(value.first.address);
    uint64_t mask = absl::Uint128High64(value.first.mask);

    TrieNode<T>* current = &head_;
    TrieNode<T>* next = current;

    while (bit & mask) {
      if (bit & addr) {
        next = current->right.get();
      } else {
        next = current->left.get();
      }

      if (next == nullptr) {
        break;
      }

      bit >>= 1;
      current = next;

      if (bit == 0) {
        // Breaks the while loop if we have traversed both high and low 64 bits.
        if (++count == 2) {
          break;
        }

        // Resets bit and starts from low 64 bits.
        bit = high_bit;
        addr = absl::Uint128Low64(value.first.address);
        mask = absl::Uint128Low64(value.first.mask);
      }
    }

    if (next) {
      if (current->data.has_value()) {
        return absl::InternalError("Can not overwrite the data in Trie.");
      }

      current->data = value.second;
      size_++;
      return absl::OkStatus();
    }

    while (bit & mask) {
      if (bit & addr) {
        current->right = std::make_unique<TrieNode<T>>();
        next = current->right.get();
      } else {
        current->left = std::make_unique<TrieNode<T>>();
        next = current->left.get();
      }

      bit >>= 1;
      current = next;

      if (bit == 0) {
        if (++count == 2) {
          break;
        }

        bit = high_bit;
        addr = absl::Uint128Low64(value.first.address);
        mask = absl::Uint128Low64(value.first.mask);
      }
    }

    current->data = value.second;
    size_++;
    return absl::OkStatus();
  }

  absl::StatusOr<const T*> find_longest(
      const absl::uint128 address) const override {
    const uint64_t high_bit = 0x8000000000000000;
    uint64_t bit = high_bit;
    uint64_t addr = absl::Uint128High64(address);
    const TrieNode<T>* current = &head_;
    const T* ptr = nullptr;

    while (current) {
      if (current->data.has_value()) {
        ptr = &current->data.value();
      }

      if (addr & bit) {
        current = current->right.get();
      } else {
        current = current->left.get();
      }

      bit >>= 1;

      if (bit == 0) {
        bit = high_bit;
        addr = absl::Uint128Low64(address);
      }
    }

    if (ptr) {
      return ptr;
    } else {
      return absl::InternalError("Fail to find a match for the given address.");
    }
  }

  bool empty() const { return size_ == 0; }

 private:
  TrieNode<T> head_;
  uint32_t size_ = 0;
};

}  // namespace isekai

#endif  // ISEKAI_OPEN_SOURCE_DEFAULT_IPV6_TRIE_H_
