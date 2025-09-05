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

#ifndef ISEKAI_HOST_RDMA_RDMA_FALCON_MODEL_H_
#define ISEKAI_HOST_RDMA_RDMA_FALCON_MODEL_H_

#include <cstdint>
#include <memory>
#include <queue>
#include <vector>

#include "absl/time/time.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/environment.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/tdigest.h"
#include "isekai/host/rdma/rdma_base_model.h"
#include "isekai/host/rdma/rdma_component_interfaces.h"
#include "isekai/host/rdma/rdma_falcon_work_scheduler.h"
#include "isekai/host/rdma/rdma_latency_histograms.pb.h"
#include "isekai/host/rdma/rdma_per_host_rx_buffers.h"
#include "isekai/host/rnic/memory_interface.h"

namespace isekai {

class RdmaFalconModel : public RdmaFalconInterface,
                        public RdmaBaseModel<FalconQpContext> {
 public:
  static constexpr double kTdigestCompression = 100.0;

  RdmaFalconModel(const RdmaConfig& config, Environment* env,
                  StatisticCollectionInterface* stats_collector,
                  ConnectionManagerInterface* connection_manager);

  void CreateRcQp(QpId local_qp_id, QpId remote_qp_id, QpOptions& options,
                  RdmaConnectedMode rc_mode) override;

  void DumpLatencyHistogramsToProto(RdmaLatencyHistograms* histograms) override;

  // Connects to a Falcon model.
  void ConnectFalcon(FalconInterface* falcon);

  // Connects to all HostInterfaces.
  void ConnectHostInterface(
      std::vector<std::unique_ptr<MemoryInterface>>* host_interface);

  void HandleRxTransaction(std::unique_ptr<Packet> packet,
                           std::unique_ptr<OpaqueCookie> cookie) override;
  void HandleCompletion(QpId qp_id, uint32_t rsn, Packet::Syndrome syndrome,
                        uint8_t destination_bifurcation_id) override;
  void ReturnFalconCredit(QpId qp_id, const FalconCredit& credit) override;

  void SetXoff(bool request_xoff, bool global_xoff) override;

  void BackpressureQP(QpId qp_id, BackpressureType type,
                      BackpressureData data) override;

  // Return a pointer to the work scheduler (used in testing if credits
  // corresponding to RDMA managed resources are handled correctly).
  RdmaFalconRoundRobinWorkScheduler* GetWorkSchedulerHandleForTesting() {
    return &work_scheduler_;
  }
  void IncreaseFalconCreditLimitForTesting(QpId qp_id,
                                           const FalconCredit& credit);

 protected:
  friend class RdmaFalconModelPeer;
  void InitializeQpContext(BaseQpContext* base_context, QpId local_qp_id,
                           QpOptions& options) override;
  // Processes an incoming packet on the given qp context.
  void ProcessRxTransaction(FalconQpContext* context, std::unique_ptr<Packet> p,
                            std::unique_ptr<OpaqueCookie> cookie);
  // Processes an incoming completion on the given qp context.
  void ProcessCompletion(FalconQpContext* context, uint32_t rsn,
                         Packet::Syndrome syndrome,
                         uint8_t destination_bifurcation_id);
  // Process a work (transaction or completion) from pipeline.
  void PipelineDequeue();
  void PostOp(FalconQpContext* qp_context, RdmaOp op) override;

  void ProcessIncomingSend(FalconQpContext* context,
                           std::unique_ptr<Packet> packet,
                           std::unique_ptr<OpaqueCookie> cookie);
  void ProcessIncomingWrite(FalconQpContext* context,
                            std::unique_ptr<Packet> packet,
                            std::unique_ptr<OpaqueCookie> cookie);
  void ProcessIncomingReadRequest(FalconQpContext* context,
                                  std::unique_ptr<Packet> packet,
                                  std::unique_ptr<OpaqueCookie> cookie);
  void ProcessIncomingReadResponse(FalconQpContext* context,
                                   std::unique_ptr<Packet> packet,
                                   std::unique_ptr<OpaqueCookie> cookie);
  // If an RSN is acceptable (any RSN for unordered, and HoL RSN for ordered).
  bool IsRsnAcceptable(FalconQpContext* context, uint32_t rsn) const;
  // Update the next RSN to receive.
  void UpdateNextRsnToReceive(FalconQpContext* context);
  void EnterRnrState(FalconQpContext* context);
  void LeaveRnrState(FalconQpContext* context);
  // If the write should be randomly RNR-NACKed.
  bool ShouldRandomRnrNackWrite() const;
  // If the read req should be randomly RNR-NACKed.
  bool ShouldRandomRnrNackRead() const;
  void CollectOpStats(FalconQpContext* context, uint32_t rsn);

  FalconInterface* falcon_ = nullptr;

  RdmaFalconRoundRobinWorkScheduler work_scheduler_;

  struct PipelineWork {
    bool is_completion;
    std::unique_ptr<Packet> packet;
    uint32_t rsn;
    Packet::Syndrome syndrome;
    FalconQpContext* context;
    uint8_t destination_bifurcation_id;
    std::unique_ptr<OpaqueCookie> cookie;
  };
  std::queue<PipelineWork> pipeline_;
  // If the next pipeline is scheduled or not.
  bool pipeline_active_ = false;
  absl::Duration pipeline_last_run_time = -absl::InfiniteDuration();

  // Histograms to measure various RDMA latency statistics.
  std::unique_ptr<TDigest> op_total_latency_;  // Op post time to completion.
  std::unique_ptr<TDigest> op_transport_latency_;  // Op start (first packet to
                                                   // Falcon) to completion.
  std::unique_ptr<TDigest> op_queueing_latency_;  // Op post to op start time.
  // The RNR timeout.
  absl::Duration rnr_timeout_ = absl::ZeroDuration();
  // The random probability of RNR NACK to write.
  double write_random_rnr_probability_ = 0;
  // The random probability of RNR NACK to read req.
  double read_random_rnr_probability_ = 0;
  // RDMA Rx buffer interface.
  std::unique_ptr<RdmaPerHostRxBuffers> rdma_per_host_rx_buffers_;
};

}  // namespace isekai

#endif  // ISEKAI_HOST_RDMA_RDMA_FALCON_MODEL_H_
