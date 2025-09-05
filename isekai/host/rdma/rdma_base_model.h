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

#ifndef ISEKAI_HOST_RDMA_RDMA_BASE_MODEL_H_
#define ISEKAI_HOST_RDMA_RDMA_BASE_MODEL_H_

#include <cstdint>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/string_view.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/host/rdma/rdma_component_interfaces.h"
#include "isekai/host/rdma/rdma_latency_histograms.pb.h"
#include "isekai/host/rdma/rdma_qp_manager.h"

namespace isekai {

enum class RdmaType {
  kFalconRdma,
};

template <typename QpContext>
class RdmaBaseModel : public RdmaBaseInterface {
 public:
  RdmaBaseModel(RdmaType rdma_type, const RdmaConfig& config, Environment* env,
                StatisticCollectionInterface* stats_collector,
                ConnectionManagerInterface* connection_manager);

  const RdmaConfig& GetConfig() override { return config_; }

  ConnectionManagerInterface* get_connection_manager() const {
    return connection_manager_;
  }

  // Schedules an RDMA op (from a traffic generator) with a scatter-gather list,
  // specified as a list of fragment lengths. The scatter-gather list may be
  // empty. Fails if there is no QP with the given source qp_id. dest_qp_id is
  // needed if QP is in UD mode. Note that sgl is pass-by-value; consider
  // std::move()ing it.
  void PerformOp(QpId qp_id, RdmaOpcode opcode, std::vector<uint32_t> sgl,
                 bool is_inline, CompletionCallback completion_callback,
                 QpId dest_qp_id) override;

  // Associates (src_qp_id, dst_qp_id) with scid.
  void Associate(QpId src_qp_id, QpId dst_qp_id, uint32_t scid) override;

  void DumpLatencyHistogramsToProto(
      RdmaLatencyHistograms* histograms) override {};

  // Collection methods.
  void CollectVectorStats(std::string_view stat_name, double value);
  void CollectScalarStats(std::string_view stat_name, double value);

 protected:
  virtual void InitializeQpContext(BaseQpContext* base_context,
                                   QpId local_qp_id, QpOptions& options);

  // Schedules an RDMA op on the given QP context.
  virtual void PostOp(QpContext* qp_context, RdmaOp op);

  const RdmaConfig config_;
  Environment* const env_;

  RdmaQpManagerInfiniteResources qp_manager_;
  ConnectionManagerInterface* connection_manager_ = nullptr;

  // Mapping from (src_qp_id, dst_qp_id) to scid.
  absl::flat_hash_map<std::pair<QpId, QpId>, uint32_t> qp_to_scid_;

  // Stats collector and flags.
  StatisticCollectionInterface* const stats_collector_;
  static StatisticsCollectionConfig::RdmaFlags stats_collection_flags_;

  // Maximum size of the data payload inside an RDMA op.
  uint32_t rdma_mtu_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_RDMA_RDMA_BASE_MODEL_H_
