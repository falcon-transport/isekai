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

#ifndef ISEKAI_HOST_FALCON_FALCON_BITMAP_H_
#define ISEKAI_HOST_FALCON_FALCON_BITMAP_H_

#include <bitset>
#include <cstddef>
#include <string>

namespace isekai {

class FalconBitmapInterface {
 public:
  FalconBitmapInterface() = default;
  virtual ~FalconBitmapInterface() = default;

  FalconBitmapInterface& operator=(
      const FalconBitmapInterface& another_bitmap) = default;

  virtual int Size() const = 0;
  // Sets the value at the passed bitset position.
  virtual void Set(int index, bool value) = 0;
  // Returns the value at the passed bitset position.
  virtual bool Get(int index) const = 0;
  // Right shifts the bitmap by the said amount.
  virtual void RightShift(int shift_amount) = 0;
  // Returns the first hole index.
  virtual int FirstHoleIndex() const = 0;
  // Check if the bitmap is empty or not.
  virtual bool Empty() const = 0;
  // Return a string representation of this bitmap. From LSB to MSB.
  virtual std::string ToString() const = 0;
};

// Represents the two kinds of bitmaps - one for request window and another for
// data window.
enum class WindowType {
  kRequest,
  kData,
};

template <size_t N>
class FalconBitmap : public FalconBitmapInterface {
 public:
  // Sizes the bitmap.
  explicit FalconBitmap(int size);
  // Copy assignment operator of the class.
  FalconBitmap<N>& operator=(const FalconBitmap<N>& another_bitmap);
  // Copies bitmap_ from a larger FalconBitmap. Only copies [0, self.size_) from
  // another_bitmap. If another_bitmap has a smaller bitmap size of self.size_,
  // report error.
  template <size_t M>
  FalconBitmap<N>& operator=(const FalconBitmap<M>& another_bitmap);
  // Returns the size of the bitmap (the valid part of the bitset structure).
  int Size() const override;
  // Sets the value at the passed bitset position.
  void Set(int index, bool value) override;
  // Returns the value at the passed bitset position.
  bool Get(int index) const override;
  // Right shifts the bitmap by the said amount.
  void RightShift(int shift_amount) override;
  // Bitwise OR on corresponding pairs of bits of *this and another_bitmap.
  void Or(const FalconBitmap& another_bitmap);
  // Returns the first hole index.
  int FirstHoleIndex() const override;
  // Check if the bitmap is empty or not.
  bool Empty() const override;
  // Return a string representation of this bitmap. From LSB to MSB.
  std::string ToString() const override;

 private:
  const int size_;
  std::bitset<N> bitmap_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_FALCON_BITMAP_H_
