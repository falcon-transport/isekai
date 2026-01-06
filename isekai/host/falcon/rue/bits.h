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

#ifndef ISEKAI_HOST_FALCON_RUE_BITS_H_
#define ISEKAI_HOST_FALCON_RUE_BITS_H_

#include <limits.h>

#include <cstdint>
#include <type_traits>

#include "absl/log/check.h"

namespace falcon_rue {

// Bit-widths for fields in Event and Response format.
// Common fields in both Gen_1 and Gen_2.
inline constexpr uint8_t kCcMetadataBits = 22;
inline constexpr uint8_t kConnectionIdBits = 24;
inline constexpr uint8_t kDelaySelectBits = 2;
inline constexpr uint8_t kEcnCounterBits = 10;
inline constexpr uint8_t kEventQueueSelectBits = 2;
inline constexpr uint8_t kEventTypeBits = 2;
inline constexpr uint8_t kFabricCongestionWindowBits = 21;
inline constexpr uint8_t kFractionalBits = 10;
inline constexpr uint8_t kForwardHopsBits = 4;
inline constexpr uint8_t kInterPacketGapBits = 20;
inline constexpr uint8_t kNackCodeBits = 8;
inline constexpr uint8_t kNumPacketsAckedBits = 10;
inline constexpr uint8_t kRetransmitReasonBits = 1;
inline constexpr uint8_t kRxBufferLevelBits = 5;
inline constexpr uint8_t kTimeBits = 24;
inline constexpr uint8_t kWindowDirectionBits = 1;
inline constexpr uint8_t kBitsOfPlbAckCounter = 10;
inline constexpr uint8_t kBitsOfPlbAttemptCounter = 4;
inline constexpr uint8_t kEcnAccumulatedBits = 14;
inline constexpr uint8_t kEackOwnBits = 2;

// Fields that were modified from Gen_1 to Gen_2.
inline constexpr uint8_t kGen1CcOpaqueBits = 2;
inline constexpr uint8_t kGen1NicCongestionWindowBits = 11;
inline constexpr uint8_t kGen1RetransmitCountBits = 3;
inline constexpr uint8_t kGen1CcMetadataBits = 22;

inline constexpr uint8_t kGen2CcOpaqueBits = 18;
inline constexpr uint8_t kGen2NicCongestionWindowBits = 21;
inline constexpr uint8_t kGen2RetransmitCountBits = 8;
inline constexpr uint8_t kGen2CcMetadataBits = 17;

// New fields added in Gen_2.
inline constexpr uint8_t kPlbStateBits = 24;
inline constexpr uint8_t kCsigBits = 16;
inline constexpr uint8_t kCsigSelectBits = 3;
inline constexpr uint8_t kArRateBits = 4;
inline constexpr uint8_t kFlowIdBits = 2;
inline constexpr uint8_t kFlowLabelBits = 20;
inline constexpr uint8_t kFlowLabelWeightBits = 4;
inline constexpr uint8_t kPerConnectionBackpressureAlphaBits = 4;
inline constexpr uint8_t kConnectionQueueCountBits = 14;
inline constexpr uint8_t kPerHostTargetOccupancyBufferLevelBits = 5;

// Fields that were modified from Gen_2 to Gen_3.
inline constexpr uint8_t kGen3RetransmitReasonBits = 2;
// New fields added in Gen_3.
inline constexpr uint8_t kCwndCarryoverBits = 2;

// Creates a bit mask.
// Output is constexpr so that compile-time definitions can be created.
// Example: Mask<uint8>(6) -> 0x3F
// Warning: if 'bits' is greater than the number of bits used by U, all ones is
// returned.
template <typename U>
constexpr U ValidMask(uint8_t bits) {
  static_assert(std::is_unsigned<U>::value);
  if (bits >= sizeof(U) * CHAR_BIT) {
    return ~static_cast<U>(0);  // all ones
  }

  return (static_cast<U>(1) << bits) - 1;
}

// Creates a bit mask.
// Output is constexpr so that compile-time definitions can be created.
// Example: InvMask<uint8>(6) -> 0xc0
// Warning: if 'bits' is greater than the number of bits used by U, all zeros is
// returned.
template <typename U>
constexpr U InvalidMask(uint8_t bits) {
  return ~ValidMask<U>(bits);
}

// Performs a mask that remove invalid bits.
// Output is constexpr so that compile-time definitions can be created.
// Examples:
// ValidMask<uint32>(8, 257) -> 1
// ValidMask<uint32>(8, 511) -> 255
template <typename U>
constexpr U ValidBits(uint8_t bits, U num) {
  return ValidMask<U>(bits) & num;
}

// Output is constexpr so that compile-time definitions can be created.
template <typename U>
constexpr U TimeBits(U num) {
  static_assert(kTimeBits < (sizeof(U) * CHAR_BIT));
  return ValidBits<U>(kTimeBits, num);
}

// Output is constexpr so that compile-time definitions can be created.
template <typename U>
constexpr U InterPacketGapBits(U num) {
  static_assert(kInterPacketGapBits < (sizeof(U) * CHAR_BIT));
  return ValidBits<U>(kInterPacketGapBits, num);
}

// Performs an inverse mask that detects when values are using more bits
// than intended. This is designed to be compared to zero and can be used in a
// CHECK() statement.
// Output is constexpr so that compile-time definitions can be created.
// Examples:
// CHECK_EQ(0, InvalidBits<uint32>(11, 1234) -> passes
// CHECK_EQ(0, InvalidBits<uint32>(12, 1234) -> passes
// CHECK_EQ(0, InvalidBits<uint32>(10, 1234) -> fails, uses more than 10 bits
// See CheckBits() below.
template <typename U>
constexpr U InvalidBits(uint8_t bits, U num) {
  return InvalidMask<U>(bits) & num;
}

// Checks that the number has not used too many bits.
// 'n' must be an unsigned value.
template <typename U>
void CheckBits(uint8_t bits, U num) {
  CHECK_EQ(0, falcon_rue::InvalidBits(bits, num));
}

// Saturates the number as a maximum if it has overflown the maximum specified
// number of bits. Undefined behaviour if bits >= bitsof(num).
// Output is constexpr so that compile-time definitions can be created.
template <typename U>
constexpr U SaturateHigh(uint8_t bits, U num) {
  if (InvalidBits<U>(bits, num)) {
    return ValidMask<U>(bits);
  }

  return num;
}

}  // namespace falcon_rue

#endif  // ISEKAI_HOST_FALCON_RUE_BITS_H_
