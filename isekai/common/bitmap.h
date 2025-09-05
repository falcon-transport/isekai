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

#ifndef ISEKAI_OPEN_SOURCE_DEFAULT_BITMAP_H_
#define ISEKAI_OPEN_SOURCE_DEFAULT_BITMAP_H_

#include <cstddef>

namespace isekai {
// Simple Bitmap class that supports 4 basic operations: (1) `get`; (2)
// `set`; (3) `clear`; and (4) `bits`.
class Bitmap {
 public:
  // Creates a bitmap with 0 bit.
  Bitmap() : nbits_(0), buffer_(nullptr) {};

  // Creates a bit map with `n` bits with all 0s.
  explicit Bitmap(size_t n) : Bitmap() { Reset(n); }

  ~Bitmap() { delete[] buffer_; };

  // Reinitialize the bitmap with `n` bits and all set to 0s.
  void Reset(size_t n);

  // Return if the ith bit is set or not. Report error if i >= nbits_.
  bool get(size_t i) const;

  // Set the ith bit. Report error if i >= nbits_.
  void set(size_t i);

  // Clear the ith bit. Report error if i >= nbits_.
  void clear(size_t i);

  // Return the number of bits held by the bitmap.
  size_t bits() const { return nbits_; }

 private:
  size_t nbits_;
  // An array of char to store n bits. Each char (1 byte) can hold 8 bits.
  unsigned char* buffer_;
};

}  // namespace isekai

#endif  // ISEKAI_OPEN_SOURCE_DEFAULT_BITMAP_H_
