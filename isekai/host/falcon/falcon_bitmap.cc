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

#include <cstddef>
#include <string>

#include "absl/log/check.h"
#include "glog/logging.h"
#include "isekai/common/constants.h"

namespace isekai {

// Instantiates the bitmap based on the size.
template <size_t N>
FalconBitmap<N>::FalconBitmap(int size) : size_(size) {}

// Copy assignment operator of FalconBitmap.
template <size_t N>
FalconBitmap<N>& FalconBitmap<N>::operator=(
    const FalconBitmap<N>& another_bitmap) {
  bitmap_ = another_bitmap.bitmap_;
  return *this;
}

// Copies bitmap_ from a larger FalconBitmap. Only copies [0, self.size_) from
// another_bitmap. If another_bitmap has a smaller bitmap size of self.size_,
// report error.
template <size_t N>
template <size_t M>
FalconBitmap<N>& FalconBitmap<N>::operator=(
    const FalconBitmap<M>& another_bitmap) {
  if (another_bitmap.Size() < size_) {
    LOG(FATAL) << "Length of source bitmap (" << M
               << ") should be >= target bitmap size (" << N << ").";
  } else {
    for (int i = 0; i < size_; ++i) {
      bitmap_[i] = another_bitmap.Get(i);
    }
  }
  return *this;
}

// Returns the size of the bitmap (the valid part of the bitset structure).
template <size_t N>
int FalconBitmap<N>::Size() const {
  return size_;
}

// Returns the value present at the passed position in the bitmap.
template <size_t N>
bool FalconBitmap<N>::Get(int index) const {
  CHECK(index >= 0 && index < Size()) << "Bitmap index out of bounds";
  return bitmap_[index];
}

// Sets the passed value at the passed position in the bitmap.
template <size_t N>
void FalconBitmap<N>::Set(int index, bool value) {
  CHECK(index >= 0 && index < Size()) << "Bitmap index out of bounds";
  bitmap_[index] = value;
}

// Right shifts the bitmap by the said amount.
template <size_t N>
void FalconBitmap<N>::RightShift(int shift_amount) {
  bitmap_ >>= shift_amount;
}

// Binary OR-s bitmap with the passed bitmap.
template <size_t N>
void FalconBitmap<N>::Or(const FalconBitmap<N>& another_bitmap) {
  bitmap_ |= another_bitmap.bitmap_;
}

// Find the first hole index in the bitmap.
template <size_t N>
int FalconBitmap<N>::FirstHoleIndex() const {
  int index = 0;
  // Get the number of 1's.
  int n = bitmap_.count();
  // first hole must appear in the first n bits, if there is hole.
  for (index = 0; index < n; index++) {
    if (!bitmap_[index]) break;
  }
  // Didn't find a hole, return -1.
  if (index == n) index = -1;
  return index;
}

template <size_t N>
bool FalconBitmap<N>::Empty() const {
  return bitmap_.none();
}

template <size_t N>
std::string FalconBitmap<N>::ToString() const {
  std::string s;
  for (int i = 0; i < N; i++) {
    s += bitmap_[i] ? '1' : '0';
  }
  return s;
}

template class FalconBitmap<kRxBitmapWidth>;
template class FalconBitmap<kTxBitmapWidth>;
template class FalconBitmap<kGen2RxBitmapWidth>;
template FalconBitmap<kAckPacketBitmapWidth>&
FalconBitmap<kAckPacketBitmapWidth>::operator=(
    const FalconBitmap<kGen2RxBitmapWidth>& another_bitmap);
}  // namespace isekai
