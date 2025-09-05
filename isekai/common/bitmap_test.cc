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

#include "isekai/common/bitmap.h"

#include <cstddef>

#include "gtest/gtest.h"

namespace {

TEST(BitmapTest, BitmapInit) {
  isekai::Bitmap map;
  EXPECT_EQ(map.bits(), 0);

  const size_t bits = 100;
  map.Reset(bits);
  EXPECT_EQ(map.bits(), bits);
  for (int i = 0; i < bits; ++i) {
    EXPECT_EQ(map.get(i), false);
  }
}

TEST(BitmapTest, BitmapOperations) {
  const size_t bits = 100;
  isekai::Bitmap map(bits);

  const size_t index = 10;
  // Test set op
  map.set(index);
  // Test get op
  EXPECT_EQ(map.get(index), true);
  // Test clear op
  map.clear(index);
  EXPECT_EQ(map.get(index), false);
}

}  // namespace
