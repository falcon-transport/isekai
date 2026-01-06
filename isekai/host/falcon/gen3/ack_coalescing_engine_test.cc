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

#include "isekai/host/falcon/gen3/ack_coalescing_engine.h"

#include <memory>

#include "glog/logging.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/constants.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/model_interfaces.h"
#include "isekai/common/packet.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/falcon_bitmap.h"
#include "isekai/host/falcon/falcon_connection_state.h"
#include "isekai/host/falcon/falcon_testing_helpers.h"
#include "isekai/host/falcon/gen1/falcon_types.h"

namespace isekai {
namespace {

// Parameterized test with scalable window enabled/disabled option.
class Gen3AckCoalescingEngineTest
    : public FalconTestingHelpers::FalconTestSetup,
      public ::testing::TestWithParam</*rx_window_size*/ int> {};

// Helper to manually allocate ReceiverReliabilityMetadata bitmaps when
// ReceiverReliabilityMetadata is not created in a ConnectionState.
void AllocateReceiveBitmaps(
    ConnectionState::ReceiverReliabilityMetadata& rx_reliability_metadata,
    int rx_window_size) {
  switch (rx_window_size) {
    case 0:
    case kGen2RxBitmapWidth:
      rx_reliability_metadata.request_window_metadata.receive_window =
          std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
      rx_reliability_metadata.request_window_metadata.ack_window =
          std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
      rx_reliability_metadata.data_window_metadata.receive_window =
          std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
      rx_reliability_metadata.data_window_metadata.ack_window =
          std::make_unique<Gen2FalconRxBitmap>(kGen2RxBitmapWidth);
      break;
    case kGen3RxBitmapWidth:
      rx_reliability_metadata.request_window_metadata.receive_window =
          std::make_unique<Gen3FalconRxBitmap>(kGen3RxBitmapWidth);
      rx_reliability_metadata.request_window_metadata.ack_window =
          std::make_unique<Gen3FalconRxBitmap>(kGen3RxBitmapWidth);
      rx_reliability_metadata.data_window_metadata.receive_window =
          std::make_unique<Gen3FalconRxBitmap>(kGen3RxBitmapWidth);
      rx_reliability_metadata.data_window_metadata.ack_window =
          std::make_unique<Gen3FalconRxBitmap>(kGen3RxBitmapWidth);
      break;
    default:
      LOG(FATAL) << "Unsupported RX window size in config: " << rx_window_size;
  }
}

TEST_P(Gen3AckCoalescingEngineTest, Gen3FillAckBitmap) {
  constexpr int kFalconVersion = 3;
  int rx_window_size = GetParam();
  FalconConfig config =
      DefaultConfigGenerator::DefaultFalconConfig(kFalconVersion);
  config.mutable_gen3_config_options()->set_rx_window_size(rx_window_size);
  InitFalcon(config);

  Gen3AckCoalescingEngine* gen3_ack_coalescing_engine =
      static_cast<Gen3AckCoalescingEngine*>(ack_coalescing_engine_);

  // Prepare rx_window data.
  ConnectionState::ReceiverReliabilityMetadata rx_reliability_metadata;
  AllocateReceiveBitmaps(rx_reliability_metadata, rx_window_size);
  rx_reliability_metadata.request_window_metadata.ack_window->Set(1, 1);
  rx_reliability_metadata.request_window_metadata.ack_window->Set(129, 1);
  rx_reliability_metadata.data_window_metadata.ack_window->Set(2, 1);
  rx_reliability_metadata.data_window_metadata.receive_window->Set(3, 1);

  // Fill ack packet and check the results.
  auto ack_packet = std::make_unique<Packet>();
  gen3_ack_coalescing_engine->FillInAckBitmaps(ack_packet.get(),
                                               rx_reliability_metadata);
  EXPECT_EQ(ack_packet->ack.receiver_request_bitmap.Get(1), 1);
  EXPECT_EQ(ack_packet->ack.receiver_data_bitmap.Get(2), 1);
  EXPECT_EQ(ack_packet->ack.received_bitmap.Get(3), 1);
}

INSTANTIATE_TEST_SUITE_P(Gen3AckCoalescingEngineTests,
                         Gen3AckCoalescingEngineTest,
                         ::testing::Values(0, kGen2RxBitmapWidth,
                                           kGen3RxBitmapWidth));

}  // namespace
}  // namespace isekai
