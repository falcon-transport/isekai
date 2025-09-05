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

#include "isekai/host/falcon/rue/fixed.h"

#include <cstdint>

#include "gtest/gtest.h"

TEST(Fixed, FloatConversion) {
  double d;
  uint64_t u;

  d = falcon_rue::FixedToFloat<uint64_t, double>(1024, 10);
  EXPECT_NEAR(d, 1.0, 0.0001);
  u = falcon_rue::FloatToFixed<double, uint64_t>(1.0, 10);
  EXPECT_EQ(u, 1024);

  d = falcon_rue::FixedToFloat<uint64_t, double>(512, 10);
  EXPECT_NEAR(d, 0.5, 0.0001);
  u = falcon_rue::FloatToFixed<double, uint64_t>(0.5, 10);
  EXPECT_EQ(u, 512);

  d = falcon_rue::FixedToFloat<uint64_t, double>(1536, 10);
  EXPECT_NEAR(d, 1.5, 0.0001);
  u = falcon_rue::FloatToFixed<double, uint64_t>(1.5, 10);
  EXPECT_EQ(u, 1536);
}

TEST(Fixed, UintConversion) {
  uint32_t v;
  uint64_t u;

  v = falcon_rue::FixedToUint<uint64_t, uint32_t>(1024, 10);
  EXPECT_EQ(v, 1u);
  u = falcon_rue::UintToFixed<uint32_t, uint64_t>(1, 10);
  EXPECT_EQ(u, 1024u);

  // Floor operation makes zeros
  v = falcon_rue::FixedToUint<uint64_t, uint32_t>(512, 10);
  EXPECT_EQ(v, 0u);
  u = falcon_rue::UintToFixed<uint32_t, uint64_t>(0, 10);
  EXPECT_EQ(u, 0u);

  // Truncation of too large of bits
  v = falcon_rue::FixedToUint<uint64_t, uint32_t>(uint64_t{0xFFFFFFFFFFFFFFFF},
                                                  10);
  EXPECT_EQ(v, 0xFFFFFFFFlu);
  u = falcon_rue::UintToFixed<uint32_t, uint64_t>(0xFFFFFFFFlu, 10);
  EXPECT_EQ(u, uint64_t{0x3fffffffc00});

  // Floor operation drops fractional bits
  v = falcon_rue::FixedToUint<uint64_t, uint32_t>(1536, 10);
  EXPECT_EQ(v, 1);
  u = falcon_rue::UintToFixed<uint32_t, uint64_t>(1, 10);
  EXPECT_EQ(u, 1024);

  // Check bounds error of UintToFixed
  u = falcon_rue::UintToFixed<uint32_t, uint64_t>(1lu << 31, 10);
  EXPECT_EQ(u, uint64_t{1} << 41);
}

TEST(Fixed, Arithmetic) {
  uint32_t a, b, c;

  // These tests assume 11-bit integer, 10-bit fractional.
  // All arithmetic fits within 32-bits

  // Addition tests
  a = falcon_rue::FloatToFixed<double, uint32_t>(3.75, 10);
  b = falcon_rue::FloatToFixed<double, uint32_t>(5.25, 10);
  c = falcon_rue::FloatToFixed<double, uint32_t>(9.0, 10);
  EXPECT_EQ(a + b, c);

  a = falcon_rue::FloatToFixed<double, uint32_t>(0.375, 10);
  b = falcon_rue::FloatToFixed<double, uint32_t>(5.00, 10);
  c = falcon_rue::FloatToFixed<double, uint32_t>(5.375, 10);
  EXPECT_EQ(a + b, c);

  // Subtraction tests
  a = falcon_rue::FloatToFixed<double, uint32_t>(5.25, 10);
  b = falcon_rue::FloatToFixed<double, uint32_t>(3.75, 10);
  c = falcon_rue::FloatToFixed<double, uint32_t>(1.50, 10);
  EXPECT_EQ(a - b, c);

  a = falcon_rue::FloatToFixed<double, uint32_t>(5.00, 10);
  b = falcon_rue::FloatToFixed<double, uint32_t>(0.375, 10);
  c = falcon_rue::FloatToFixed<double, uint32_t>(4.625, 10);
  EXPECT_EQ(a - b, c);

  // Multiplication tests
  a = falcon_rue::FloatToFixed<double, uint32_t>(5.00, 10);
  b = falcon_rue::FloatToFixed<double, uint32_t>(2.00, 10);
  c = falcon_rue::FloatToFixed<double, uint32_t>(10.0, 10);
  EXPECT_EQ((a * b) >> 10, c);

  a = falcon_rue::FloatToFixed<double, uint32_t>(5.00, 10);
  b = falcon_rue::FloatToFixed<double, uint32_t>(0.50, 10);
  c = falcon_rue::FloatToFixed<double, uint32_t>(2.5, 10);
  EXPECT_EQ((a * b) >> 10, c);

  a = falcon_rue::FloatToFixed<double, uint32_t>(1025, 10);
  b = falcon_rue::FloatToFixed<double, uint32_t>(2.50, 10);
  c = falcon_rue::FloatToFixed<double, uint32_t>(2562.5, 10);
  EXPECT_EQ((a * b) >> 10, c);

  // Division tests
  a = falcon_rue::FloatToFixed<double, uint32_t>(1025, 10);
  b = falcon_rue::FloatToFixed<double, uint32_t>(2.00, 10);
  c = falcon_rue::FloatToFixed<double, uint32_t>(512.5, 10);
  EXPECT_EQ((a << 10) / b, c);

  a = falcon_rue::FloatToFixed<double, uint32_t>(0.5, 10);
  b = falcon_rue::FloatToFixed<double, uint32_t>(2.0, 10);
  c = falcon_rue::FloatToFixed<double, uint32_t>(0.25, 10);
  EXPECT_EQ((a << 10) / b, c);

  a = falcon_rue::FloatToFixed<double, uint32_t>(1.0, 10);
  b = falcon_rue::FloatToFixed<double, uint32_t>(3.0, 10);
  c = falcon_rue::FloatToFixed<double, uint32_t>(0.3333, 10);
  EXPECT_EQ((a << 10) / b, c);
}
