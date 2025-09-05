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

#include "isekai/host/falcon/falcon_bitmap.h"

#include "gtest/gtest.h"

namespace isekai {

namespace {

TEST(FalconBitmapTest, BitmapInitialization) {
  FalconBitmap<128> request_bitmap(64);
  EXPECT_EQ(request_bitmap.Size(), 64);

  FalconBitmap<128> data_bitmap(128);
  EXPECT_EQ(data_bitmap.Size(), 128);
  FalconBitmap<128> falconb(64);
}

TEST(FalconBitmapTest, BitmapSetGet) {
  FalconBitmap<128> request_bitmap(64);
  EXPECT_EQ(request_bitmap.Size(), 64);
  request_bitmap.Set(0, true);
  EXPECT_EQ(request_bitmap.Get(0), true);
}

TEST(FalconBitmapTest, BitmapCopyAssignment) {
  FalconBitmap<128> bitmap(64);
  bitmap.Set(0, true);
  FalconBitmap<128> another_bitmap = bitmap;
  EXPECT_EQ(another_bitmap.Get(0), true);
}

TEST(FalconBitmapTest, BitmapOr) {
  FalconBitmap<128> bitmap(64);
  bitmap.Set(0, true);
  FalconBitmap<128> another_bitmap(64);
  another_bitmap.Set(1, true);
  bitmap.Or(another_bitmap);
  EXPECT_EQ(bitmap.Get(1), true);
}

TEST(FalconBitmapTest, BitmapUpdate) {
  // Bitmap update from bitmaps with same size of bitset.
  // Use case of Gen1 Falcon ack generation.
  FalconBitmap<128> small_source(8);
  FalconBitmap<128> small_target(4);
  small_source.Set(1, 1);  // source = "01000000" (LSB -> MSB)
  small_target = small_source;
  EXPECT_EQ(small_target.Size(), 4);
  EXPECT_EQ(small_target.Get(1), 1);  // expected target: "0100" (LSG -> MSB)

  // Bitmap update from a bitmap with larger bitset,
  // Use case of Gen2 Falcon ack generation.
  FalconBitmap<256> source(8);
  FalconBitmap<128> target(4);
  source.Set(1, 1);  // source = "01000000" (LSB -> MSB)
  target = source;
  EXPECT_EQ(target.Size(), 4);
  EXPECT_EQ(target.Get(1), 1);  // expected target: "0100" (LSG -> MSB)
}

}  // namespace
}  // namespace isekai
