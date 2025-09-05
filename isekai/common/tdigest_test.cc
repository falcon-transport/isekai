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

#include "isekai/common/tdigest.h"

#include <memory>

#include "glog/logging.h"
#include "gtest/gtest.h"
#include "isekai/common/tdigest.pb.h"

namespace isekai {
namespace {

TEST(TDigest, TestAdd) {
  auto tdigest = TDigest::New(100);
  for (int i = 0; i < 1000; ++i) {
    tdigest->Add(i);
  }
  ASSERT_EQ(tdigest->Min(), 0);
  ASSERT_EQ(tdigest->Max(), 999);
  ASSERT_EQ(tdigest->Count(), 1000);
  ASSERT_EQ(tdigest->Sum(), 999 * 1000 / 2);
  ASSERT_NEAR(tdigest->Quantile(0.5), 499, 1);
}

TEST(TDigest, TestToProto) {
  auto tdigest = TDigest::New(100);
  tdigest->Add(100);
  proto::TDigest tdigest_proto;
  tdigest->ToProto(&tdigest_proto);
  // LOG(INFO) << tdigest_proto;
  EXPECT_EQ(tdigest_proto.count(), 1);
  EXPECT_EQ(tdigest_proto.max(), 100);
  EXPECT_EQ(tdigest_proto.min(), 100);
  EXPECT_EQ(tdigest_proto.sum(), 100);
  EXPECT_EQ(tdigest_proto.centroid_size(), 1);
  EXPECT_EQ(tdigest_proto.centroid(0).mean(), 100);
  EXPECT_EQ(tdigest_proto.centroid(0).weight(), 1);
}

}  // namespace
}  // namespace isekai
