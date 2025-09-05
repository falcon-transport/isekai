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

#include "isekai/host/rnic/memory_interface.h"

#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/simple_environment.h"
#include "isekai/common/testing.h"

namespace isekai {
namespace {

//
// abstraction (and moved logic from MemoryInterface to MemoryInterfaceQueue),
// the tests in this file need to be changed to test the abstraction.
TEST(MemoryInterfaceTest, TestReadFromMemoryWithConstDelay) {
  SimpleEnvironment env;
  MemoryInterfaceConfig options;
  options.mutable_read_queue_config()->set_bandwidth_bps(8e3);
  options.mutable_read_queue_config()->set_memory_delay_const_ns(2e9);
  options.mutable_read_queue_config()->set_delay_distribution(
      MemoryInterfaceConfig_MemoryDelayDistribution_CONST);
  options.mutable_read_queue_config()->set_memory_interface_queue_size_packets(
      1);
  MemoryInterface h(options, &env);

  EXPECT_OK(h.CanReadFromMemory());
  h.ReadFromMemory(
      /*size=*/2000, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Seconds(4)); });

  EXPECT_OK(h.CanReadFromMemory());
  h.ReadFromMemory(
      /*size=*/2000, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Seconds(6)); });

  EXPECT_TRUE(absl::IsResourceExhausted(h.CanReadFromMemory()));
  absl::Duration delay = absl::Seconds(6);
  EXPECT_OK(env.ScheduleEvent(delay, [&]() {
    EXPECT_OK(h.CanReadFromMemory());
    h.ReadFromMemory(/*size=*/1000,
                     [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Seconds(9)); });
  }));
  env.Run();
}

TEST(MemoryInterfaceTest, TestWriteToMemoryWithConstDelay) {
  SimpleEnvironment env;
  MemoryInterfaceConfig options;
  options.mutable_write_queue_config()->set_bandwidth_bps(8e3);
  options.mutable_write_queue_config()->set_memory_delay_const_ns(2e9);
  options.mutable_write_queue_config()->set_delay_distribution(
      MemoryInterfaceConfig_MemoryDelayDistribution_CONST);
  options.mutable_write_queue_config()->set_memory_interface_queue_size_packets(
      1);
  MemoryInterface h(options, &env);

  EXPECT_OK(h.CanWriteToMemory());
  h.WriteToMemory(
      /*size=*/2000, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Seconds(4)); });

  EXPECT_OK(h.CanWriteToMemory());
  h.WriteToMemory(
      /*size=*/2000, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Seconds(6)); });

  EXPECT_TRUE(absl::IsResourceExhausted(h.CanWriteToMemory()));
  absl::Duration delay = absl::Seconds(6);
  EXPECT_OK(env.ScheduleEvent(delay, [&]() {
    EXPECT_OK(h.CanWriteToMemory());
    h.WriteToMemory(/*size=*/1000,
                    [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Seconds(9)); });
  }));
  env.Run();
}

TEST(MemoryInterfaceTest, TestWriteToMemoryWakeupCallback) {
  SimpleEnvironment env;
  MemoryInterfaceConfig options;
  options.mutable_write_queue_config()->set_bandwidth_bps(8e3);
  options.mutable_write_queue_config()->set_memory_delay_const_ns(2e9);
  options.mutable_write_queue_config()->set_delay_distribution(
      MemoryInterfaceConfig_MemoryDelayDistribution_CONST);
  options.mutable_write_queue_config()->set_memory_interface_queue_size_packets(
      1);
  MemoryInterface h(options, &env);
  h.SetWriteCallback([&]() {
    static bool send_once = true;
    if (send_once) {
      EXPECT_OK(h.CanWriteToMemory());
      h.WriteToMemory(
          /*size=*/1000, [&]() {
            EXPECT_EQ(env.ElapsedTime(), absl::Seconds(/*elapsed time*/ 1 +
                                                       /*previous packet*/
                                                       2 +
                                                       /*transmission time
                                                        * for this packet
                                                        */
                                                       1 +
                                                       /* link delay */ 2));
          });
      send_once = false;
    }
  });

  EXPECT_OK(h.CanWriteToMemory());
  h.WriteToMemory(
      /*size=*/1000, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Seconds(3)); });

  EXPECT_OK(h.CanWriteToMemory());
  h.WriteToMemory(
      /*size=*/2000, [&]() {
        EXPECT_EQ(env.ElapsedTime(), absl::Seconds(/*previous packet*/ 1 +
                                                   /*time to transmit this
                                                    * packet */
                                                   2 +
                                                   /* link delay */ 2));
      });

  EXPECT_TRUE(absl::IsResourceExhausted(h.CanWriteToMemory()));
  env.Run();
}

TEST(MemoryInterfaceTest, TestWriteToMemoryWithUniformDelay) {
  int n_pkts = 10000;
  SimpleEnvironment env;
  MemoryInterfaceConfig options;
  options.mutable_write_queue_config()->set_bandwidth_bps(0);
  options.mutable_write_queue_config()->set_memory_delay_uniform_low_ns(10);
  options.mutable_write_queue_config()->set_memory_delay_uniform_high_ns(19);
  options.mutable_write_queue_config()->set_delay_distribution(
      MemoryInterfaceConfig_MemoryDelayDistribution_UNIFORM);
  options.mutable_write_queue_config()->set_memory_interface_queue_size_packets(
      n_pkts + 1);
  MemoryInterface h(options, &env);

  std::vector<absl::Duration> completion_times;
  for (int i = 0; i < n_pkts; i++) {
    EXPECT_OK(h.CanWriteToMemory());
    h.WriteToMemory(
        /*size=*/2000,
        [&]() { completion_times.push_back(env.ElapsedTime()); });
  }

  env.RunFor(absl::Nanoseconds(100000));
  EXPECT_EQ(completion_times.size(), n_pkts);

  std::map<absl::Duration, int> histo;
  for (const auto& t : completion_times) histo[t] = histo[t] + 1;

  EXPECT_EQ(histo.size(),
            options.write_queue_config().memory_delay_uniform_high_ns() -
                options.write_queue_config().memory_delay_uniform_low_ns() + 1);

  for (const auto& count : histo)
    EXPECT_NEAR(count.second, n_pkts / histo.size(),
                n_pkts / histo.size() / 10);
}

TEST(MemoryInterfaceTest, TestWriteToMemoryWithExponentialDelay) {
  int n_pkts = 10000;
  SimpleEnvironment env;
  MemoryInterfaceConfig options;
  options.mutable_write_queue_config()->set_bandwidth_bps(0);
  options.mutable_write_queue_config()->set_memory_delay_exponential_mean_ns(
      10);
  options.mutable_write_queue_config()->set_delay_distribution(
      MemoryInterfaceConfig_MemoryDelayDistribution_EXPONENTIAL);
  options.mutable_write_queue_config()->set_memory_interface_queue_size_packets(
      n_pkts + 1);
  MemoryInterface h(options, &env);

  std::vector<absl::Duration> completion_times;
  for (int i = 0; i < n_pkts; i++) {
    EXPECT_OK(h.CanWriteToMemory());
    h.WriteToMemory(
        /*size=*/2000,
        [&]() { completion_times.push_back(env.ElapsedTime()); });
  }

  env.RunFor(absl::Nanoseconds(1000000));
  EXPECT_EQ(completion_times.size(), n_pkts);

  std::map<int64_t, int> histo;
  for (const auto& time : completion_times) {
    int64_t t = absl::ToInt64Nanoseconds(time);
    histo[t] = histo[t] + 1;
  }

  // Test mean
  double mean = 0;
  for (const auto& count : histo) {
    mean += count.first * count.second;
  }
  mean /= n_pkts;
  EXPECT_NEAR(mean,
              options.write_queue_config().memory_delay_exponential_mean_ns(),
              mean * 0.1);

  // Test variance
  double var = 0;
  for (const auto& count : histo)
    var += (count.first - mean) * (count.first - mean) * count.second;
  var /= (n_pkts - 1);
  EXPECT_NEAR(
      var,
      options.write_queue_config().memory_delay_exponential_mean_ns() *
          options.write_queue_config().memory_delay_exponential_mean_ns(),
      options.write_queue_config().memory_delay_exponential_mean_ns() *
          options.write_queue_config().memory_delay_exponential_mean_ns() *
          0.1);
}

TEST(MemoryInterfaceTest, TestWriteToMemoryWithDiscreteDelay) {
  int n_pkts = 10000;
  SimpleEnvironment env;
  MemoryInterfaceConfig options;
  options.mutable_write_queue_config()->set_bandwidth_bps(0);
  options.mutable_write_queue_config()->set_memory_delay_discrete_mapping(
      "1:1,3:2,5:3,7:4");
  options.mutable_write_queue_config()->set_delay_distribution(
      MemoryInterfaceConfig_MemoryDelayDistribution_DISCRETE);
  options.mutable_write_queue_config()->set_memory_interface_queue_size_packets(
      n_pkts + 1);
  MemoryInterface h(options, &env);

  std::vector<absl::Duration> completion_times;
  for (int i = 0; i < n_pkts; i++) {
    EXPECT_OK(h.CanWriteToMemory());
    h.WriteToMemory(
        /*size=*/2000,
        [&]() { completion_times.push_back(env.ElapsedTime()); });
  }

  env.RunFor(absl::Nanoseconds(100000));
  EXPECT_EQ(completion_times.size(), n_pkts);

  std::map<int64_t, int> histo;
  for (const auto& time : completion_times) {
    int64_t t = absl::ToInt64Nanoseconds(time);
    histo[t] = histo[t] + 1;
  }

  EXPECT_EQ(histo.size(), 4);
  EXPECT_TRUE(histo.find(1) != histo.end());
  EXPECT_TRUE(histo.find(3) != histo.end());
  EXPECT_TRUE(histo.find(5) != histo.end());
  EXPECT_TRUE(histo.find(7) != histo.end());
  EXPECT_NEAR(histo[1], n_pkts * 1.0 / 10, n_pkts * 1.0 / 10 * 0.05);
  EXPECT_NEAR(histo[3], n_pkts * 2.0 / 10, n_pkts * 2.0 / 10 * 0.05);
  EXPECT_NEAR(histo[5], n_pkts * 3.0 / 10, n_pkts * 3.0 / 10 * 0.05);
  EXPECT_NEAR(histo[7], n_pkts * 4.0 / 10, n_pkts * 4.0 / 10 * 0.05);
}

TEST(MemoryInterfaceTest, TestInfiniteFastPcie) {
  SimpleEnvironment env;
  MemoryInterfaceConfig options;
  options.mutable_write_queue_config()->set_memory_interface_queue_size_packets(
      1);
  MemoryInterface h(options, &env);

  EXPECT_OK(h.CanWriteToMemory());
  h.WriteToMemory(
      /*size=*/1000, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Seconds(0)); });

  absl::Duration delay1 = absl::Seconds(1);
  EXPECT_OK(env.ScheduleEvent(delay1, [&]() {
    EXPECT_OK(h.CanWriteToMemory());
    h.WriteToMemory(/*size=*/1000,
                    [&]() { EXPECT_EQ(env.ElapsedTime(), delay1); });
  }));

  absl::Duration delay2 = absl::Seconds(2);
  EXPECT_OK(env.ScheduleEvent(delay2, [&]() {
    EXPECT_OK(h.CanWriteToMemory());
    h.WriteToMemory(/*size=*/1000,
                    [&]() { EXPECT_EQ(env.ElapsedTime(), delay2); });
  }));

  absl::Duration delay3 = absl::Seconds(3);
  EXPECT_OK(env.ScheduleEvent(delay3, [&]() {
    EXPECT_OK(h.CanWriteToMemory());
    h.WriteToMemory(/*size=*/1000,
                    [&]() { EXPECT_EQ(env.ElapsedTime(), delay3); });
  }));
  env.Run();
}

}  // namespace
}  // namespace isekai
