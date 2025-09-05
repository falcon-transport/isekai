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

#include <vector>

#include "folly/stats/TDigest.h"
#include "isekai/common/tdigest.pb.h"

namespace isekai {

void TDigest::Add(double val) {
  std::vector<double> range = {val};
  tdigest_ = tdigest_.merge(range);
}

void TDigest::ToProto(proto::TDigest* proto) {
  proto->Clear();

  proto->set_count(tdigest_.count());
  proto->set_max(tdigest_.max());
  proto->set_min(tdigest_.min());
  proto->set_sum(tdigest_.sum());

  for (const auto& centroid : tdigest_.getCentroids()) {
    auto* new_centroid = proto->add_centroid();
    new_centroid->set_mean(centroid.mean());
    new_centroid->set_weight(centroid.weight());
  }
}

}  // namespace isekai
