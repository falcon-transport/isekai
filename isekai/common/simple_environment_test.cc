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

#include "isekai/common/simple_environment.h"

#include <string>
#include <vector>

#include "absl/time/time.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/testing.h"

namespace isekai {
namespace {

using testing::ElementsAre;

TEST(SimpleEnvironmentTest, ScheduleEvent) {
  SimpleEnvironment env;
  std::vector<std::string> log;

  EXPECT_OK(env.ScheduleEvent(absl::Seconds(2), [&]() {
    log.push_back("b");
    EXPECT_OK(
        env.ScheduleEvent(absl::Seconds(2), [&]() { log.push_back("d"); }));
  }));
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(3), [&]() { log.push_back("c"); }));
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(1), [&]() { log.push_back("a"); }));

  env.Run();
  EXPECT_THAT(log, ElementsAre("a", "b", "c", "d"));
}

TEST(SimpleEnvironmentTest, ElapsedTime) {
  SimpleEnvironment env;
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(2), [&]() {
    EXPECT_EQ(env.ElapsedTime(), absl::Seconds(2));
    EXPECT_OK(env.ScheduleEvent(absl::Seconds(3), [&]() {
      EXPECT_EQ(env.ElapsedTime(), absl::Seconds(5));
    }));
  }));

  env.Run();
}

TEST(SimpleEnvironmentTest, Stop) {
  SimpleEnvironment env;
  int counter = 0;
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(1), [&]() { ++counter; }));
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(2), [&]() { ++counter; }));
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(3), [&]() { env.Stop(); }));
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(4), [&]() { ++counter; }));
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(5), [&]() { ++counter; }));

  // env.Stop() should break out of this loop at 3 seconds.
  env.Run();

  EXPECT_EQ(counter, 2);
  EXPECT_EQ(env.ElapsedTime(), absl::Seconds(3));
}

TEST(SimpleEnvironmentTest, StableOrder) {
  // Schedule multiple events at the same time, and verify they are executed in
  // the order they were scheduled.
  SimpleEnvironment env;
  std::vector<int> log;

  for (int i = 0; i < 10; ++i) {
    EXPECT_OK(
        env.ScheduleEvent(absl::Seconds(0), [i, &log]() { log.push_back(i); }));
    EXPECT_OK(env.ScheduleEvent(absl::Seconds(1),
                                [i, &log]() { log.push_back(i + 10); }));
  }

  env.Run();

  EXPECT_THAT(log, ElementsAre(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                               15, 16, 17, 18, 19));
}

TEST(SimpleEnvironmentTest, RunAndPause) {
  SimpleEnvironment env;
  std::vector<std::string> log;

  // a @ 1s -> e b @ 2s -> c @ 3s -> f d @ 4s -> g @ 5s
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(2), [&]() { log.push_back("e"); }));
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(2), [&]() {
    log.push_back("b");
    EXPECT_OK(
        env.ScheduleEvent(absl::Seconds(2), [&]() { log.push_back("d"); }));
  }));
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(3), [&]() { log.push_back("c"); }));
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(1), [&]() { log.push_back("a"); }));
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(4), [&]() { log.push_back("f"); }));
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(5), [&]() { log.push_back("g"); }));

  env.RunFor(absl::Seconds(1));
  EXPECT_THAT(log, ElementsAre("a"));

  env.RunUntil(absl::Seconds(2));
  EXPECT_THAT(log, ElementsAre("a", "e", "b"));

  env.RunFor(absl::Seconds(1));
  EXPECT_THAT(log, ElementsAre("a", "e", "b", "c"));

  env.RunFor(absl::Seconds(1));
  EXPECT_THAT(log, ElementsAre("a", "e", "b", "c", "f", "d"));

  env.RunUntil(absl::Seconds(10));
  EXPECT_THAT(log, ElementsAre("a", "e", "b", "c", "f", "d", "g"));
}

TEST(SimpleEnvironmentTest, TestRunUtil) {
  SimpleEnvironment env;
  std::vector<std::string> log;

  // a @ 1s -> e b @ 2s -> c @ 3s -> d @ 4s
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(2), [&]() { log.push_back("e"); }));
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(2), [&]() {
    log.push_back("b");
    EXPECT_OK(
        env.ScheduleEvent(absl::Seconds(2), [&]() { log.push_back("d"); }));
  }));
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(3), [&]() { log.push_back("c"); }));
  EXPECT_OK(env.ScheduleEvent(absl::Seconds(1), [&]() { log.push_back("a"); }));

  env.RunFor(absl::Seconds(1));
  EXPECT_THAT(log, ElementsAre("a"));

  env.RunFor(absl::Seconds(1));
  EXPECT_THAT(log, ElementsAre("a", "e", "b"));

  env.RunFor(absl::Seconds(1));
  EXPECT_THAT(log, ElementsAre("a", "e", "b", "c"));

  env.RunFor(absl::Seconds(1));
  EXPECT_THAT(log, ElementsAre("a", "e", "b", "c", "d"));
}

TEST(SimpleEnvironmentTest, NumEvents) {
  SimpleEnvironment env;
  int counter = 0;

  EXPECT_EQ(env.ScheduledEvents(), 0);
  EXPECT_EQ(env.ExecutedEvents(), 0);

  EXPECT_OK(env.ScheduleEvent(absl::Seconds(1), [&]() { ++counter; }));

  EXPECT_EQ(env.ScheduledEvents(), 1);
  EXPECT_EQ(env.ExecutedEvents(), 0);

  env.Run();

  EXPECT_EQ(env.ScheduledEvents(), 1);
  EXPECT_EQ(env.ExecutedEvents(), 1);

  EXPECT_OK(env.ScheduleEvent(absl::Seconds(1), [&]() { ++counter; }));

  EXPECT_EQ(env.ScheduledEvents(), 2);
  EXPECT_EQ(env.ExecutedEvents(), 1);

  env.Run();

  EXPECT_EQ(env.ScheduledEvents(), 2);
  EXPECT_EQ(env.ExecutedEvents(), 2);
}

TEST(SimpleEnvironmentTest, ElaspedTimeDelta) {
  SimpleEnvironment env;

  absl::Duration event_durations[] = {
      absl::Nanoseconds(1),  absl::Nanoseconds(1.01), absl::Microseconds(100),
      absl::Seconds(1.2345), absl::Seconds(5),        absl::Seconds(100)};

  absl::Duration future_duration = absl::Seconds(100000);

  for (auto& duration : event_durations) {
    EXPECT_OK(env.ScheduleEvent(duration, [&, duration]() {
      // At the scheduled event, ElapsedTimeEquals should evaluate true with the
      // duration provided to ScheduleEvent.
      EXPECT_TRUE(env.ElapsedTimeEquals(duration));
      EXPECT_FALSE(env.ElapsedTimeEquals(future_duration));
    }));
  }

  env.RunFor(future_duration);
}

}  // namespace
}  // namespace isekai
