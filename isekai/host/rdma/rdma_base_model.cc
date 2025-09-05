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

#include "isekai/host/rdma/rdma_base_model.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "glog/logging.h"
#include "isekai/common/common_util.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/net_address.h"
#include "isekai/common/status_util.h"
#include "isekai/host/rdma/rdma_component_interfaces.h"
#include "isekai/host/rdma/rdma_qp_manager.h"

namespace isekai {

template <typename QpContext>
StatisticsCollectionConfig::RdmaFlags
    RdmaBaseModel<QpContext>::stats_collection_flags_;

template <typename QpContext>
RdmaBaseModel<QpContext>::RdmaBaseModel(
    RdmaType rdma_type, const RdmaConfig& config, Environment* env,
    StatisticCollectionInterface* stats_collector,
    ConnectionManagerInterface* connection_manager)
    : config_(config),
      env_(env),
      qp_manager_(env_),
      connection_manager_(connection_manager),
      stats_collector_(stats_collector),
      rdma_mtu_(config_.mtu()) {
  if (stats_collector_ != nullptr &&
      stats_collector_->GetConfig().has_rdma_flags()) {
    stats_collection_flags_ = stats_collector_->GetConfig().rdma_flags();
  } else {
    stats_collection_flags_ = DefaultConfigGenerator::DefaultRdmaStatsFlags();
  }
}

template <typename QpContext>
void RdmaBaseModel<QpContext>::InitializeQpContext(BaseQpContext* base_context,
                                                   QpId local_qp_id,
                                                   QpOptions& options) {
  base_context->qp_id = local_qp_id;
  CHECK_OK_THEN_ASSIGN(base_context->dst_ip,
                       Ipv6Address::OfString(options.dst_ip));
}

template <typename QpContext>
void RdmaBaseModel<QpContext>::PerformOp(QpId qp_id, RdmaOpcode opcode,
                                         std::vector<uint32_t> sgl,
                                         bool is_inline,
                                         CompletionCallback completion_callback,
                                         QpId dest_qp_id) {
  uint32_t total_payload_length = 0;
  for (uint32_t segment_length : sgl) {
    CHECK(segment_length <= config_.max_segment_length() && segment_length > 0)
        << "Segment of " << segment_length << " bytes is too big or 0";
    total_payload_length += segment_length;
  }

  std::unique_ptr<RdmaOp> rdma_op;
  if (!is_inline || opcode == RdmaOpcode::kRead ||
      opcode == RdmaOpcode::kRecv) {
    CHECK(!is_inline) << "RDMA read and recv op cannot be inline.";
    rdma_op = std::make_unique<RdmaOp>(
        opcode, std::move(sgl), std::move(completion_callback), dest_qp_id);
  } else {
    CHECK(total_payload_length <= config_.max_inline_payload_length())
        << "Inline payload of " << total_payload_length << " bytes is too big";
    rdma_op =
        std::make_unique<RdmaOp>(opcode, total_payload_length,
                                 std::move(completion_callback), dest_qp_id);
  }

  // Initiate qp lookup, and when it finishes, perform further qp processing.
  qp_manager_.InitiateQpLookup(
      qp_id, [this, op = *rdma_op](absl::StatusOr<BaseQpContext*> qp_context) {
        CHECK_OK(qp_context.status())
            << "Unknown QP id: " << qp_context.status();
        PostOp(down_cast<QpContext*>(qp_context.value()), std::move(op));
      });
}

template <typename QpContext>
void RdmaBaseModel<QpContext>::Associate(QpId src_qp_id, QpId dst_qp_id,
                                         uint32_t scid) {
  qp_to_scid_[std::make_pair(src_qp_id, dst_qp_id)] = scid;
}

template <typename QpContext>
void RdmaBaseModel<QpContext>::PostOp(QpContext* context, RdmaOp op) {
  if (context->is_connected()) {
    op.dest_qp_id = context->dest_qp_id;
  } else {
    CHECK(op.opcode == RdmaOpcode::kSend || op.opcode == RdmaOpcode::kRecv)
        << "Unsupported verb on UD QP.";
  }

  if (op.opcode == RdmaOpcode::kRecv) {
    context->receive_queue.push_back(std::make_unique<RdmaOp>(op));
  } else {
    context->send_queue.push_back(std::make_unique<RdmaOp>(op));
  }
}

template <typename QpContext>
void RdmaBaseModel<QpContext>::CollectVectorStats(std::string_view stat_name,
                                                  double value) {
  if (stats_collector_) {
    CHECK_OK(stats_collector_->UpdateStatistic(
        stat_name, value, StatisticsCollectionConfig::TIME_SERIES_STAT));
  }
}

template <typename QpContext>
void RdmaBaseModel<QpContext>::CollectScalarStats(std::string_view stat_name,
                                                  double value) {
  if (stats_collector_) {
    CHECK_OK(stats_collector_->UpdateStatistic(
        stat_name, value, StatisticsCollectionConfig::SCALAR_MAX_STAT));
  }
}

template class RdmaBaseModel<FalconQpContext>;

}  // namespace isekai
