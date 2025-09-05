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
#include <cstring>

#include "glog/logging.h"

namespace isekai {
namespace {

inline size_t GetBytesFromBits(size_t n) { return (n + 7) / 8; }

}  // namespace

bool Bitmap::get(size_t i) const {
  if (i >= nbits_) {
    LOG(FATAL) << "Invalid index for bitmap: " << i;
  }

  return buffer_[i / 8] & (1u << (i & 7)) ? true : false;
}

void Bitmap::set(size_t i) {
  if (i >= nbits_) {
    LOG(FATAL) << "Invalid index for bitmap: " << i;
  }

  buffer_[i / 8] |= 1u << (i & 7);
}

void Bitmap::clear(size_t i) {
  if (i >= nbits_) {
    LOG(FATAL) << "Invalid index for bitmap: " << i;
  }

  buffer_[i / 8] &= ~(1u << (i & 7));
}

void Bitmap::Reset(size_t n) {
  size_t required_bytes = GetBytesFromBits(n);
  if (required_bytes != GetBytesFromBits(nbits_)) {
    unsigned char* buffer = new unsigned char[required_bytes];
    delete[] buffer_;
    buffer_ = buffer;
  }
  memset(buffer_, 0, required_bytes);
  nbits_ = n;
}

}  // namespace isekai
