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

#ifndef ISEKAI_COMMON_CONSTANTS_H_
#define ISEKAI_COMMON_CONSTANTS_H_

#include <cstdint>

namespace isekai {

// RDMA constants.
constexpr uint32_t kCbthSize = 12;
constexpr uint32_t kRethSize = 16;
constexpr uint32_t kSethSize = 4;
constexpr uint32_t kOethSize = 4;
constexpr uint32_t kStethSize = 12;
constexpr uint32_t kReadHeaderSize =
    kCbthSize + kRethSize + kSethSize + kStethSize;
constexpr uint32_t kWriteHeaderSize = kCbthSize + kRethSize;
constexpr uint32_t kResponseHeaderSize = kCbthSize + kStethSize;
constexpr uint32_t kSendHeaderSize = kCbthSize + kSethSize + kOethSize;
constexpr uint32_t kSglHeaderSize = 8;
constexpr uint32_t kSglFragmentHeaderSize = 16;
// Max number of RDMA ops a WorkQueue is allowed to send before switching to
// the next WorkQueue. This limitation is because WQEs are read in batches of
// 256B and each WQE is atleast 32B, making max op quanta equal to 8.
constexpr uint32_t kMaxRdmaOpsPerQuanta = 8;
constexpr uint32_t kDefaultRdmaMtu = 4096;

// Packet header sizes.
constexpr uint32_t kEthernetHeader = 14;
constexpr uint32_t kIpv6Header = 40;
constexpr uint32_t kUdpHeader = 8;
constexpr uint32_t kPspHeader = 24;
constexpr uint32_t kFalconHeader = 24;
constexpr uint32_t kFalconOpHeader = 12;
constexpr uint32_t kRdmaHeader = 28;
constexpr uint32_t kPspTrailer = 16;
constexpr uint32_t kEthernetPreambleFCS = 12;
constexpr uint32_t kOuterRdmaHeaderSize = kFalconHeader + kFalconOpHeader +
                                          kUdpHeader + kIpv6Header +
                                          kEthernetHeader;
constexpr uint32_t kNetworkMtu = 9000;

// Bitmap sizes for pkt/tx/rx request/data window.
// For bitmaps of ack packet and rx window, it can have different sizes for
// request and window. We uniformly use one size so that other functions do not
// need to be templated.
// Changing bitmap sizes might affect class declaration in falcon_bitmap.cc.
// In falcon_bitmap.cc FalconBitmap template class declaration, each bitmap size
// needs to be declared but only once. Currently the unique bitmap sizes include
// 128, 256 and 1024. As an example, if setting kGen2TxBitmapWidth = 512, it is
// necessary to add a class declaration with kGen2TxBitmapWidth.
constexpr int kAckPacketBitmapWidth = 128;
constexpr int kAckPacketRequestWindowSize = 64;
constexpr int kAckPacketDataWindowSize = 128;
constexpr int kRxBitmapWidth = kAckPacketBitmapWidth;
constexpr int kRxRequestWindowSize = 64;
constexpr int kRxDataWindowSize = 128;
//
constexpr int kTxBitmapWidth = 1024;
// Bitmap sizes for Falcon Gen2.
constexpr int kGen2RxBitmapWidth = 256;
constexpr int kGen2TxBitmapWidth = 1024;

// RX bitmap sizes for Falcon Gen3.
constexpr int kGen3TxBitmapWidth = 1024;
constexpr int kGen3RxBitmapWidth = 512;
}  // namespace isekai

#endif  // ISEKAI_COMMON_CONSTANTS_H_
