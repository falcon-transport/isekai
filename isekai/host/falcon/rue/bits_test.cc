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

#include "isekai/host/falcon/rue/bits.h"

#include <cstdint>

#include "gtest/gtest.h"

TEST(Util, ValidMask) {
  EXPECT_EQ(falcon_rue::ValidMask<uint32_t>(24), 0x00FFFFFF);
  EXPECT_EQ(falcon_rue::ValidMask<uint32_t>(23), 0x007FFFFF);
  EXPECT_EQ(falcon_rue::ValidMask<uint32_t>(25), 0x01FFFFFF);
  EXPECT_EQ(falcon_rue::ValidMask<uint32_t>(0), 0x0);
  EXPECT_EQ(falcon_rue::ValidMask<uint32_t>(31), 0x7FFFFFFF);
  EXPECT_EQ(falcon_rue::ValidMask<uint32_t>(32), 0xFFFFFFFF);
  EXPECT_EQ(falcon_rue::ValidMask<uint32_t>(33), 0xFFFFFFFF);  // overflow
}

TEST(Util, InvalidMask) {
  EXPECT_EQ(falcon_rue::InvalidMask<uint32_t>(24), 0xFF000000);
  EXPECT_EQ(falcon_rue::InvalidMask<uint32_t>(23), 0xFF800000);
  EXPECT_EQ(falcon_rue::InvalidMask<uint32_t>(25), 0xFE000000);
  EXPECT_EQ(falcon_rue::InvalidMask<uint32_t>(0), 0xFFFFFFFF);
  EXPECT_EQ(falcon_rue::InvalidMask<uint32_t>(31), 0x80000000);
  EXPECT_EQ(falcon_rue::InvalidMask<uint32_t>(32), 0x00000000);
  EXPECT_EQ(falcon_rue::InvalidMask<uint32_t>(33), 0x00000000);  // overflow
}

TEST(Util, ValidBits) {
  EXPECT_EQ(falcon_rue::ValidBits<uint32_t>(8, 256), 0);
  EXPECT_EQ(falcon_rue::ValidBits<uint32_t>(8, 257), 1);
  EXPECT_EQ(falcon_rue::ValidBits<uint32_t>(8, 511), 255);
  EXPECT_EQ(falcon_rue::ValidBits<uint32_t>(8, 1023), 255);
  EXPECT_EQ(falcon_rue::ValidBits<uint32_t>(8, 1024), 0);
}

TEST(Util, TimeBits) {
  EXPECT_EQ(falcon_rue::TimeBits<uint32_t>(0xFFFFFFFFu), 0x00FFFFFF);
}

TEST(Util, InterPacketGapBits) {
  EXPECT_EQ(falcon_rue::InterPacketGapBits<uint32_t>(0xFFFFFFFFu), 0x000FFFFF);
}

TEST(Util, InvalidBits) {
  EXPECT_EQ(0, falcon_rue::InvalidBits<uint32_t>(11, 1234));
  EXPECT_EQ(0, falcon_rue::InvalidBits<uint32_t>(12, 1234));
  EXPECT_NE(0, falcon_rue::InvalidBits<uint32_t>(10, 1234));

  EXPECT_EQ(0, falcon_rue::InvalidBits<uint32_t>(11, 1024));
  EXPECT_EQ(0, falcon_rue::InvalidBits<uint32_t>(12, 1024));
  EXPECT_NE(0, falcon_rue::InvalidBits<uint32_t>(10, 1024));

  EXPECT_EQ(0, falcon_rue::InvalidBits<uint32_t>(11, 1023));
  EXPECT_EQ(0, falcon_rue::InvalidBits<uint32_t>(12, 1023));
  EXPECT_EQ(0, falcon_rue::InvalidBits<uint32_t>(10, 1023));
  EXPECT_NE(0, falcon_rue::InvalidBits<uint32_t>(9, 1023));
}
