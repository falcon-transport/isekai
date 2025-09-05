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

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/time/time.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/default_config_generator.h"
#include "isekai/common/simple_environment.h"
#include "isekai/host/falcon/gen1/falcon_model.h"

namespace isekai {
namespace {

TEST(FalconInterHostRxScheduler, TestSingleHostAllArrivalsAtSameTime) {
  SimpleEnvironment env;
  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(1);
  config.set_inter_host_rx_scheduling_tick_ns(5);
  FalconModel falcon(config, &env, nullptr, nullptr, "falcon-host",
                     /* number of hosts */ 4);

  auto& scheduler = *falcon.get_inter_host_rx_scheduler();

  // Enqueue to uninitialized host.
  EXPECT_DEATH(scheduler.Enqueue(
                   5, []() { LOG(FATAL) << "This should never get called"; }),
               "");
  // Initially host is in inactive state
  EXPECT_EQ(scheduler.HasWork(), false);

  // Enqueue few items at once and expect them to execute in sequence every tick
  // (in this case 5ns)
  scheduler.Enqueue(
      0, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(5)); });
  scheduler.Enqueue(
      0, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(10)); });
  EXPECT_EQ(scheduler.HasWork(), true);

  // Schedule few items when scheduler is already running
  CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(8), [&]() {
    scheduler.Enqueue(
        0, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(15)); });
  }));
  CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(15), [&]() {
    scheduler.Enqueue(
        0, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(20)); });
  }));
  env.RunFor(absl::Nanoseconds(20));

  // Run for another to drain the queue
  env.RunFor(absl::Nanoseconds(2));

  // Again add an item, this should get executed after 5ns.
  scheduler.Enqueue(
      0, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(27)); });
  env.Run();
}

TEST(FalconInterHostRxScheduler, TestNoHostEnqueue) {
  SimpleEnvironment env;
  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(1);
  config.set_inter_host_rx_scheduling_tick_ns(5);
  FalconModel falcon(config, &env, nullptr, nullptr, "falcon-host",
                     /* number of hosts */ 4);

  auto& scheduler = *falcon.get_inter_host_rx_scheduler();

  // Enqueue to uninitialized host.
  EXPECT_DEATH(scheduler.Enqueue(
                   5, []() { LOG(FATAL) << "This should never get called"; }),
               "");
}

// Trying these configurations for Single host:
// +───────+─────────+
// | Xoff  | Active  |
// +───────+─────────+
// | True  | True    |
// | True  | False   |
// | False | True    |
// | False | False   |
// +───────+─────────+
TEST(FalconInterHostRxScheduler, TestSingleHostWithAllCombination) {
  SimpleEnvironment env;
  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(1);
  config.set_inter_host_rx_scheduling_tick_ns(5);
  FalconModel falcon(config, &env, nullptr, nullptr, "falcon-host",
                     /* number of hosts */ 4);

  auto& scheduler = *falcon.get_inter_host_rx_scheduler();

  EXPECT_EQ(scheduler.HasWork(), false);

  // Case 1
  {
    CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(0), [&]() {
      // Xoff host
      scheduler.SetXoff(0, true);
      // Enqueue an item to mark host active.
      scheduler.Enqueue(
          0, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(6)); });
    }));
    // Check after tick time if there is any work to do. The scheduler should
    // not have any work to schdedule since the host is xoffed.
    CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(5), [&]() {
      EXPECT_EQ(scheduler.HasWork(), false);
    }));

    // Xon the host so that the item gets cleared up
    CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(6),
                               [&]() { scheduler.SetXoff(0, false); }));
  }
  env.RunFor(absl::Nanoseconds(11));

  // Case 2
  {
    CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(11),
                               [&]() { scheduler.SetXoff(0, true); }));
    CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(16), [&]() {
      EXPECT_EQ(scheduler.HasWork(), false);
    }));
  }

  // Case 3
  {
    CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(20), [&]() {
      // Xoff host
      scheduler.SetXoff(0, false);
      // Enqueue an item to mark host active.
      scheduler.Enqueue(
          0, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(25)); });
    }));
  }
  env.RunFor(absl::Nanoseconds(16));

  // Case 4
  {
    CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(30), [&]() {
      // Xoff host
      scheduler.SetXoff(0, false);
    }));
    CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(35), [&]() {
      EXPECT_EQ(scheduler.HasWork(), false);
    }));
  }
  env.Run();
}

TEST(FalconInterHostRxScheduler,
     TestSingleHostCheckXoffWhenSchedulerIsRunning) {
  SimpleEnvironment env;
  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(1);
  config.set_inter_host_rx_scheduling_tick_ns(5);
  FalconModel falcon(config, &env, nullptr, nullptr, "falcon-host",
                     /* number of hosts */ 4);

  auto& scheduler = *falcon.get_inter_host_rx_scheduler();
  // Initially host is in inactive state
  EXPECT_EQ(scheduler.HasWork(), false);

  // Enqueue an item and kick in the scheduler to execute after tick amount of
  // time.
  scheduler.Enqueue(
      0, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(5)); });
  EXPECT_EQ(scheduler.HasWork(), true);

  // Enter another item but this host is going to Xoff at 8th ns and con at 11th
  // ns. So this will get executed bit later,
  scheduler.Enqueue(
      0, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(11)); });

  // Schedule an Xoff when scheduler is already running.
  CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(8),
                             [&]() { scheduler.SetXoff(0, true); }));

  // The scheduler should not have any work to schdedule since the host is
  // xoffed.
  CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(10),
                             [&]() { EXPECT_EQ(scheduler.HasWork(), false); }));

  // Schedule an Xoff when scheduler is already running.
  CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(11),
                             [&]() { scheduler.SetXoff(0, false); }));

  // The scheduler should not have any work to schedule at 12th ns, as the
  // pending work is finished immediately on Xon.
  CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(12),
                             [&]() { EXPECT_EQ(scheduler.HasWork(), false); }));

  // Schedule an item when scheduler is already running
  CHECK_OK(env.ScheduleEvent(absl::Nanoseconds(15), [&]() {
    scheduler.Enqueue(
        0, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(16)); });
  }));

  env.Run();
}

TEST(FalconInterHostRxScheduler, TestDynamicInterPacketGap) {
  SimpleEnvironment env;
  FalconConfig config = DefaultConfigGenerator::DefaultFalconConfig(1);
  config.set_inter_host_rx_scheduling_tick_ns(5);
  config.set_rx_falcon_ulp_link_gbps(200);
  FalconModel falcon(config, &env, nullptr, nullptr, "falcon-host",
                     /* number of hosts */ 4);

  auto& scheduler = *falcon.get_inter_host_rx_scheduler();
  // Initially host is in inactive state
  EXPECT_EQ(scheduler.HasWork(), false);

  // Enqueue an item and kick in the scheduler to execute after tick amount of
  // time.
  scheduler.Enqueue(0, [&]() {
    scheduler.UpdateInterPacketGap(/*packet_size=*/500);
    EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(5));
  });
  EXPECT_EQ(scheduler.HasWork(), true);

  // Enter another item but this item would be serviced 5 (elapsed time) + 20
  // (packet serialization delay) = 25
  scheduler.Enqueue(0, [&]() {
    scheduler.UpdateInterPacketGap(/*packet_size=*/1);
    EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(25));
  });

  // Enter another item but this item would be serviced 5 (elapsed time) + 20
  // (packet serialization delay) + 5 (min tick time) = 30
  scheduler.Enqueue(
      0, [&]() { EXPECT_EQ(env.ElapsedTime(), absl::Nanoseconds(30)); });

  env.Run();
}

}  // namespace
}  // namespace isekai
