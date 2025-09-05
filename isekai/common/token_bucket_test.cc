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

#include "absl/time/time.h"
#include "gtest/gtest.h"

namespace isekai {
namespace {

TEST(TokenBucketTest, TokenBucketInitialization) {
  // Creates a token bucket with rate of 10K tokens/sec, burst size of 5K tokens
  // and refill interval of 200us (i.e., 2 tokes per interval).
  TokenBucket token_bucket(10000, absl::Nanoseconds(200000), 5000);

  // Given that no time has elapsed and the bucket starts with the burst size of
  // 5K as available tokens, we expect a request of 5K to return true.
  EXPECT_TRUE(token_bucket.AreTokensAvailable(5000, absl::ZeroDuration()));
  token_bucket.RequestTokens(5000);

  // Given that the current time passed (100us) < refill interval (200us), and
  // we used up the 5K tokens, any new request cannot be satisfied.
  EXPECT_FALSE(token_bucket.AreTokensAvailable(1, absl::Nanoseconds(100000)));

  // Given that the current time passed (300us) > refill interval (200us),
  // request higher than initial tokens cannot be satisfied.
  EXPECT_TRUE(token_bucket.AreTokensAvailable(1, absl::Nanoseconds(300000)));
  token_bucket.RequestTokens(1);

  // Given that the current time passed (500 - 300us) >= refill interval
  // (200us), we expect a request of 3 tokens to be satisfied.
  EXPECT_TRUE(token_bucket.AreTokensAvailable(3, absl::Nanoseconds(500000)));
  EXPECT_FALSE(token_bucket.AreTokensAvailable(4, absl::Nanoseconds(500000)));
}

TEST(TokenBucketTest, TokenBucketCheckBetweenInterval) {
  TokenBucket token_bucket(/*tokens_per_sec=*/100,
                           /*bucket_refill_interval=*/absl::Seconds(1),
                           /*max_burst_size=*/100);

  // This updates the last refill time to 1s.
  EXPECT_TRUE(token_bucket.AreTokensAvailable(100, absl::Seconds(1)));

  // This step will reduce number of tokens to 0.
  token_bucket.RequestTokens(100);

  // This step should replenish tokens back to 100.
  EXPECT_TRUE(token_bucket.AreTokensAvailable(100, absl::Seconds(2.5)));

  // This step will reduce number of tokens to 0 again.
  token_bucket.RequestTokens(100);

  // We should have tokens available again at t = 3s.
  EXPECT_TRUE(token_bucket.AreTokensAvailable(100, absl::Seconds(3)));
}

}  // namespace
}  // namespace isekai
