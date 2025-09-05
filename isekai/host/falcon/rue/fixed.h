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

#ifndef ISEKAI_HOST_FALCON_RUE_FIXED_H_
#define ISEKAI_HOST_FALCON_RUE_FIXED_H_

#include <cmath>
#include <limits>
#include <type_traits>

namespace falcon_rue {

// Converts a fixed-point value 'num' of type U with 'frac' fractional
// bits to a floating-point value of type F.
// Output is constexpr so that compile-time definitions can be created.
template <typename U, typename F>
constexpr F FixedToFloat(U num, int frac) {
  static_assert(std::is_unsigned<U>::value);
  static_assert(std::is_floating_point<F>::value);
  const U one = 1;
  return static_cast<F>(num) / (one << frac);
}

// Converts a floating-point value 'num' of type F to a fixed-point value
// of type U with 'frac' fractional bits.
// Output is constexpr so that compile-time definitions can be created.
// WARNING: this assumes 'num' is finite, positive, and representable in U.
template <typename F, typename U>
constexpr U FloatToFixed(F num, int frac) {
  static_assert(std::is_floating_point<F>::value);
  static_assert(std::is_unsigned<U>::value);
  const U one = 1;
  F f = std::round(num * (one << frac));
  return static_cast<U>(f);
}

// Converts a fixed-point value 'num' of type U with 'frac' fractional
// bits to an unsigned integer of type V. The fractional bits are ignored
// making this effectively a floor(num) operation. Truncation occurs if
// num without 'frac' fractional bits still doesn't fit within type V.
// Output is constexpr so that compile-time definitions can be created.
template <typename U, typename V>
constexpr V FixedToUint(U num, int frac) {
  static_assert(std::is_unsigned<U>::value);
  static_assert(std::is_unsigned<V>::value);
  return static_cast<V>(num >> frac);
}

// Converts an unsigned integer value 'num' of type V to a fixed-point value
// of type U with 'frac' fractional bits. Fractional bits are set to zero. If
// 'num' is too large to fit in bitsof(U) + frac bits, truncation occurs.
// Output is constexpr so that compile-time definitions can be created.
template <typename V, typename U>
constexpr U UintToFixed(V num, int frac) {
  static_assert(std::is_unsigned<V>::value);
  static_assert(std::is_unsigned<U>::value);
  return static_cast<U>(num) << frac;
}

}  // namespace falcon_rue

#endif  // ISEKAI_HOST_FALCON_RUE_FIXED_H_
