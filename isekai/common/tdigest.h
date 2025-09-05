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

#ifndef ISEKAI_OPEN_SOURCE_DEFAULT_TDIGEST_H_
#define ISEKAI_OPEN_SOURCE_DEFAULT_TDIGEST_H_

#include <memory>

#include "folly/stats/TDigest.h"
#include "isekai/common/tdigest.pb.h"

namespace isekai {

// The class is a wrapper of folly::TDigest for open source purpose.
class TDigest {
 public:
  explicit TDigest(double compression) {
    tdigest_ = folly::TDigest(compression);
  }
  static std::unique_ptr<TDigest> New(double compression) {
    return std::make_unique<TDigest>(compression);
  }
  // Add a value to tdigest.
  void Add(double val);
  // Dump the tdigest to proto.
  void ToProto(proto::TDigest* proto);
  // `quantile` can be any real value between 0 and 1.
  inline double Quantile(double quantile) const {
    return tdigest_.estimateQuantile(quantile);
  }
  inline double Min() const { return tdigest_.min(); }
  inline double Max() const { return tdigest_.max(); }
  inline double Sum() const { return tdigest_.sum(); }
  inline double Count() const { return tdigest_.count(); }

 private:
  folly::TDigest tdigest_;
};

}  // namespace isekai

#endif  // ISEKAI_OPEN_SOURCE_DEFAULT_TDIGEST_H_
