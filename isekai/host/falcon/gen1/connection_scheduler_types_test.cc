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

#include "isekai/host/falcon/gen1/connection_scheduler_types.h"

#include "absl/container/btree_set.h"
#include "gtest/gtest.h"

namespace isekai {
namespace {

TEST(ProtocolConnectionSchedulerTypesTest, RetransmissionPriorityEnforcement) {
  RetransmissionWorkId w1(1, 1, falcon::PacketType::kPullRequest);
  RetransmissionWorkId w2(0, 2, falcon::PacketType::kPullRequest);
  absl::btree_set<RetransmissionWorkId> set;
  set.insert(w2);
  set.insert(w1);
  EXPECT_EQ(set.begin()->psn, 1);
  set.erase(set.begin());
  EXPECT_EQ(set.begin()->psn, 2);
}

}  // namespace
}  // namespace isekai
