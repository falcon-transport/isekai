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

#ifndef ISEKAI_HOST_RDMA_RDMA_QP_MANAGER_H_
#define ISEKAI_HOST_RDMA_RDMA_QP_MANAGER_H_

#include <memory>

#include "absl/container/flat_hash_map.h"
#include "absl/functional/any_invocable.h"
#include "absl/status/statusor.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/host/rdma/rdma_component_interfaces.h"

namespace isekai {

// Sub-component of the RDMA model that stores and manages all QueuePair context
// state. It does not model the QP context cache, NIC DRAM bandwidth and
// latency, effectively making it an infinitely fast RDMA model.
class RdmaQpManagerInfiniteResources : public RdmaQpManagerInterface {
 public:
  explicit RdmaQpManagerInfiniteResources(Environment* env) : env_(env) {}

  void CreateQp(std::unique_ptr<BaseQpContext> context) override;
  void ConnectQp(QpId local_qp_id, QpId remote_qp_id,
                 RdmaConnectedMode rc_mode) override;
  void InitiateQpLookup(QpId qp_id,
                        absl::AnyInvocable<void(absl::StatusOr<BaseQpContext*>)>
                            callback) override;
  BaseQpContext* DirectQpLookup(QpId qp_id) override;

 private:
  // QP contexts of all QueuePairs.
  absl::flat_hash_map<QpId, std::unique_ptr<BaseQpContext>> qp_contexts_;

  Environment* const env_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_RDMA_RDMA_QP_MANAGER_H_
