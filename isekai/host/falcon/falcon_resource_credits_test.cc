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

#include "isekai/host/falcon/falcon_resource_credits.h"

#include <type_traits>

#include "google/protobuf/text_format.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"

namespace isekai {
namespace {

const char kConfigCredits[] =
    R"pb(
  tx_packet_credits { ulp_requests: 1 ulp_data: 2 network_requests: 3 }
  tx_buffer_credits { ulp_requests: 4 ulp_data: 5 network_requests: 6 }
  rx_packet_credits { ulp_requests: 7 network_requests: 8 }
  rx_buffer_credits { ulp_requests: 9 network_requests: 10 })pb";

const FalconResourceCredits kCredits = {
    .tx_packet_credits =
        {
            .ulp_requests = 1,
            .ulp_data = 2,
            .network_requests = 3,
        },
    .tx_buffer_credits =
        {
            .ulp_requests = 4,
            .ulp_data = 5,
            .network_requests = 6,
        },
    .rx_packet_credits =
        {
            .ulp_requests = 7,
            .network_requests = 8,
        },
    .rx_buffer_credits =
        {
            .ulp_requests = 9,
            .network_requests = 10,
        },
};

TEST(FalconResourceCredits, Create) {
  FalconConfig::ResourceCredits config_credits;
  ASSERT_TRUE(google::protobuf::TextFormat::ParseFromString(kConfigCredits,
                                                            &config_credits));
  ASSERT_EQ(config_credits.tx_packet_credits().ulp_requests(), 1);
  ASSERT_EQ(config_credits.tx_packet_credits().ulp_data(), 2);
  ASSERT_EQ(config_credits.tx_packet_credits().network_requests(), 3);
  ASSERT_EQ(config_credits.tx_buffer_credits().ulp_requests(), 4);
  ASSERT_EQ(config_credits.tx_buffer_credits().ulp_data(), 5);
  ASSERT_EQ(config_credits.tx_buffer_credits().network_requests(), 6);
  ASSERT_EQ(config_credits.rx_packet_credits().ulp_requests(), 7);
  ASSERT_EQ(config_credits.rx_packet_credits().network_requests(), 8);
  ASSERT_EQ(config_credits.rx_buffer_credits().ulp_requests(), 9);
  ASSERT_EQ(config_credits.rx_buffer_credits().network_requests(), 10);

  FalconResourceCredits credits = FalconResourceCredits::Create(config_credits);
  EXPECT_EQ(credits, kCredits);
}

TEST(FalconResourceCredits, CompareEq) {
  const FalconResourceCredits eq = {
      .tx_packet_credits =
          {
              .ulp_requests = 1,
              .ulp_data = 2,
              .network_requests = 3,
          },
      .tx_buffer_credits =
          {
              .ulp_requests = 4,
              .ulp_data = 5,
              .network_requests = 6,
          },
      .rx_packet_credits =
          {
              .ulp_requests = 7,
              .network_requests = 8,
          },
      .rx_buffer_credits =
          {
              .ulp_requests = 9,
              .network_requests = 10,
          },
  };
  EXPECT_TRUE(eq <= kCredits);
  EXPECT_TRUE(eq == kCredits);
}

TEST(FalconResourceCredits, Compare2) {
  const FalconResourceCredits lt1 = {
      .tx_packet_credits =
          {
              .ulp_requests = 0,  // -1
              .ulp_data = 2,
              .network_requests = 3,
          },
      .tx_buffer_credits =
          {
              .ulp_requests = 4,
              .ulp_data = 5,
              .network_requests = 6,
          },
      .rx_packet_credits =
          {
              .ulp_requests = 7,
              .network_requests = 8,
          },
      .rx_buffer_credits =
          {
              .ulp_requests = 9,
              .network_requests = 10,
          },
  };
  EXPECT_TRUE(lt1 <= kCredits);
  EXPECT_FALSE(lt1 == kCredits);
}

TEST(FalconResourceCredits, Compare3) {
  const FalconResourceCredits lt2 = {
      .tx_packet_credits =
          {
              .ulp_requests = 1,
              .ulp_data = 2,
              .network_requests = 3,
          },
      .tx_buffer_credits =
          {
              .ulp_requests = 4,
              .ulp_data = 5,
              .network_requests = 4,  // -2
          },
      .rx_packet_credits =
          {
              .ulp_requests = 7,
              .network_requests = 8,
          },
      .rx_buffer_credits =
          {
              .ulp_requests = 9,
              .network_requests = 10,
          },
  };
  EXPECT_TRUE(lt2 <= kCredits);
  EXPECT_FALSE(lt2 == kCredits);
}

TEST(FalconResourceCredits, Compare4) {
  const FalconResourceCredits gt1 = {
      .tx_packet_credits =
          {
              .ulp_requests = 0,  // -1
              .ulp_data = 2,
              .network_requests = 3,
          },
      .tx_buffer_credits =
          {
              .ulp_requests = 4,
              .ulp_data = 5,
              .network_requests = 6,
          },
      .rx_packet_credits =
          {
              .ulp_requests = 8,  // +1
              .network_requests = 8,
          },
      .rx_buffer_credits =
          {
              .ulp_requests = 9,
              .network_requests = 10,
          },
  };
  EXPECT_FALSE(gt1 <= kCredits);
  EXPECT_FALSE(gt1 == kCredits);
}

TEST(FalconResourceCredits, Compare5) {
  const FalconResourceCredits gt2 = {
      .tx_packet_credits =
          {
              .ulp_requests = 1,
              .ulp_data = 2,
              .network_requests = 3,
          },
      .tx_buffer_credits =
          {
              .ulp_requests = 4,
              .ulp_data = 5,
              .network_requests = 9,  // +3
          },
      .rx_packet_credits =
          {
              .ulp_requests = 7,
              .network_requests = 7,  // -1
          },
      .rx_buffer_credits =
          {
              .ulp_requests = 9,
              .network_requests = 10,
          },
  };
  EXPECT_FALSE(gt2 <= kCredits);
  EXPECT_FALSE(gt2 == kCredits);
}

TEST(FalconResourceCredits, PlusEqualEmpty) {
  FalconResourceCredits credits = kCredits;
  credits += {
      .tx_packet_credits =
          {
              .ulp_requests = 0,
              .ulp_data = 0,
              .network_requests = 0,
          },
      .tx_buffer_credits =
          {
              .ulp_requests = 0,
              .ulp_data = 0,
              .network_requests = 0,
          },
      .rx_packet_credits =
          {
              .ulp_requests = 0,
              .network_requests = 0,
          },
      .rx_buffer_credits =
          {
              .ulp_requests = 0,
              .network_requests = 0,
          },
  };
  EXPECT_EQ(credits, kCredits);
}

TEST(FalconResourceCredits, MinusEqualEmpty) {
  FalconResourceCredits credits = kCredits;
  credits -= {
      .tx_packet_credits =
          {
              .ulp_requests = 0,
              .ulp_data = 0,
              .network_requests = 0,
          },
      .tx_buffer_credits =
          {
              .ulp_requests = 0,
              .ulp_data = 0,
              .network_requests = 0,
          },
      .rx_packet_credits =
          {
              .ulp_requests = 0,
              .network_requests = 0,
          },
      .rx_buffer_credits =
          {
              .ulp_requests = 0,
              .network_requests = 0,
          },
  };
  EXPECT_EQ(credits, kCredits);
}

TEST(FalconResourceCredits, PlusEqualTwoFields) {
  FalconResourceCredits credits = kCredits;
  credits += {
      .tx_packet_credits =
          {
              .ulp_requests = 0,
              .ulp_data = 0,
              .network_requests = 2,
          },
      .tx_buffer_credits =
          {
              .ulp_requests = 0,
              .ulp_data = 0,
              .network_requests = 0,
          },
      .rx_packet_credits =
          {
              .ulp_requests = 3,
              .network_requests = 0,
          },
      .rx_buffer_credits =
          {
              .ulp_requests = 0,
              .network_requests = 0,
          },
  };
  FalconResourceCredits expected = kCredits;
  expected.tx_packet_credits.network_requests += 2;
  expected.rx_packet_credits.ulp_requests += 3;
  EXPECT_EQ(credits, expected);
}

TEST(FalconResourceCredits, MinusEqualTwoFields) {
  FalconResourceCredits credits = kCredits;
  credits -= {
      .tx_packet_credits =
          {
              .ulp_requests = 0,
              .ulp_data = 0,
              .network_requests = 2,
          },
      .tx_buffer_credits =
          {
              .ulp_requests = 0,
              .ulp_data = 0,
              .network_requests = 0,
          },
      .rx_packet_credits =
          {
              .ulp_requests = 3,
              .network_requests = 0,
          },
      .rx_buffer_credits =
          {
              .ulp_requests = 0,
              .network_requests = 0,
          },
  };
  FalconResourceCredits expected = kCredits;
  expected.tx_packet_credits.network_requests -= 2;
  expected.rx_packet_credits.ulp_requests -= 3;
  EXPECT_EQ(credits, expected);
}

}  // namespace
}  // namespace isekai
