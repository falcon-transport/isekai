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

#include "isekai/common/token_bucket.h"

#include <cmath>
#include <cstdint>

#include "absl/log/check.h"
#include "absl/time/time.h"

namespace isekai {

TokenBucket::TokenBucket(uint64_t tokens_per_sec,
                         absl::Duration bucket_refill_interval,
                         uint64_t max_burst_size)
    : tokens_per_refill_interval_(
          tokens_per_sec *
          absl::FDivDuration(bucket_refill_interval, absl::Seconds(1))),
      bucket_refill_interval_(bucket_refill_interval),
      bucket_size_(max_burst_size),
      available_tokens_(max_burst_size),
      last_refill_timestamp_(absl::ZeroDuration()) {
  CHECK_GT(tokens_per_refill_interval_, 0);
}

// Verifies if the bucket has the request number of tokens.
bool TokenBucket::AreTokensAvailable(uint64_t requested_tokens,
                                     absl::Duration current_time) {
  // Calculate the number of refill intervals elapsed since the last refill.
  int bucket_refill_intervals_elapsed = std::floor(absl::FDivDuration(
      current_time - last_refill_timestamp_, bucket_refill_interval_));

  if (bucket_refill_intervals_elapsed > 0) {
    // Update the number of available tokens in the bucket.
    available_tokens_ = std::min(
        available_tokens_ +
            (bucket_refill_intervals_elapsed * tokens_per_refill_interval_),
        bucket_size_);
    last_refill_timestamp_ +=
        bucket_refill_intervals_elapsed * bucket_refill_interval_;
  }
  return requested_tokens <= available_tokens_;
}

// Reduces the number of tokens per the input parameters.
void TokenBucket::RequestTokens(uint64_t required_tokens) {
  // This method should be called only after the AreTokensAvailable method.
  CHECK_GE(available_tokens_, required_tokens);
  available_tokens_ -= required_tokens;
}

}  // namespace isekai
