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

#include "isekai/host/falcon/rue/algorithm/dram_state_manager.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "absl/log/check.h"
#include "gtest/gtest.h"
#include "isekai/common/testing.h"
#include "isekai/host/falcon/rue/format_gen1.h"
#include "isekai/host/falcon/rue/format_gen2.h"

namespace isekai {
namespace rue {
namespace {

template <typename TypeParam>
class DramStateManagerTest : public ::testing::Test {
 public:
  using ManagerT = TypeParam;
  using EventT = typename ManagerT::EventType;
  static auto CreateManager(size_t per_connection_state_size);
  EventT dummy_event_{};
};

template <typename TypeParam>
auto DramStateManagerTest<TypeParam>::CreateManager(
    size_t per_connection_state_size) {
  // Some data structures allocate a large amount of memory upfront. For the
  // purposes of testing, our maximum number of connections can be lower.
  const uint8_t kMaxConnectionBits = 5;
  const size_t kMaxConnections = 1 << kMaxConnectionBits;

  if constexpr (std::is_same_v<ManagerT, SingletonDramStateManager<EventT>>) {
    return std::make_unique<SingletonDramStateManager<EventT>>(
        per_connection_state_size);
  } else if constexpr (std::is_same_v<ManagerT,
                                      ConnectionBitsDramStateManager<EventT>>) {
    return std::make_unique<ConnectionBitsDramStateManager<EventT>>(
        per_connection_state_size, kMaxConnectionBits);
  } else if constexpr (std::is_same_v<
                           ManagerT,
                           TableWithOffsetInEventDramStateManager<EventT>>) {
    return std::make_unique<TableWithOffsetInEventDramStateManager<EventT>>(
        per_connection_state_size, kMaxConnections);
  } else if constexpr (std::is_same_v<ManagerT,
                                      HashMapDramStateManager<EventT>>) {
    return std::make_unique<HashMapDramStateManager<EventT>>(
        per_connection_state_size, kMaxConnections);
  }
}

// Used for tests that are common to all Dram State Managers.
using DramStateManagerTypes = ::testing::Types<
    SingletonDramStateManager<falcon_rue::Event_GEN1>,
    ConnectionBitsDramStateManager<falcon_rue::Event_GEN1>,
    TableWithOffsetInEventDramStateManager<falcon_rue::Event_GEN1>>;

TYPED_TEST_SUITE(DramStateManagerTest, DramStateManagerTypes);

TYPED_TEST(DramStateManagerTest, FailsOnStateSizeZero) {
  EXPECT_DEATH(this->CreateManager(0), "Check failed");
}

TYPED_TEST(DramStateManagerTest, EnsureSlcSliceBoundaryAlignment) {
  auto manager = this->CreateManager(64);
  auto& state = manager->template GetStateForEvent<int>(this->dummy_event_);
  uintptr_t addr = reinterpret_cast<uintptr_t>(std::addressof(state));
  EXPECT_EQ(addr % kSlcSliceBoundary, 0);
}

TYPED_TEST(DramStateManagerTest, SatisfiesBasicContract) {
  struct PerConnectionStateT {
    int x;
  };

  auto manager = this->CreateManager(sizeof(PerConnectionStateT));

  this->dummy_event_.connection_id = 1;
  auto& state1 = manager->template GetStateForEvent<PerConnectionStateT>(
      this->dummy_event_);
  state1.x = 1;

  auto& state2 = manager->template GetStateForEvent<PerConnectionStateT>(
      this->dummy_event_);
  EXPECT_EQ(state1.x, state2.x);
  EXPECT_EQ(std::addressof(state1), std::addressof(state2));
}

// Helper function to perform standard checks on retrieved state on structure
// whose size is known at compile time (template parameter `size`).
template <typename ManagerT, size_t size>
static void CheckValuesAndReturnedAddressForSameConnectionId(
    DramStateManagerTest<ManagerT>& fixture) {
  struct PerConnectionStateT {
    char arr[size];
  };

  using StateT = PerConnectionStateT;
  auto manager = fixture.CreateManager(sizeof(PerConnectionStateT));
  auto& state1 =
      manager->template GetStateForEvent<StateT>(fixture.dummy_event_);
  for (int i = 0; i < sizeof(StateT::arr); ++i) {
    state1.arr[i] = i % 255;
  }

  uintptr_t addr = reinterpret_cast<uintptr_t>(std::addressof(state1));
  EXPECT_EQ(addr % kSlcSliceBoundary, 0);

  auto& state2 =
      manager->template GetStateForEvent<StateT>(fixture.dummy_event_);
  for (int i = 0; i < sizeof(StateT::arr); ++i) {
    EXPECT_EQ(state1.arr[i], state2.arr[i]);
  }
  EXPECT_EQ(std::addressof(state1), std::addressof(state2));
}

TYPED_TEST(DramStateManagerTest, WorksWithDifferentSizes) {
  using ManagerT = TypeParam;
  CheckValuesAndReturnedAddressForSameConnectionId<ManagerT, 1>(*this);
  CheckValuesAndReturnedAddressForSameConnectionId<ManagerT, 4>(*this);
  CheckValuesAndReturnedAddressForSameConnectionId<ManagerT, 32>(*this);
  CheckValuesAndReturnedAddressForSameConnectionId<ManagerT, 64>(*this);
  CheckValuesAndReturnedAddressForSameConnectionId<ManagerT, 78>(*this);
  CheckValuesAndReturnedAddressForSameConnectionId<ManagerT, 128>(*this);
  CheckValuesAndReturnedAddressForSameConnectionId<ManagerT, 129>(*this);
}

// The GetStateForEvent is templated on a state type (eventually the per
// connection state to be held) and returns a reference of that type. However,
// for benchmarking, the state type (and specifically the size of the per
// connection state) is not known at compile time. The benchmarking code knows
// the size of the state only at runtime via command line flags, and therefore
// must typecast the returned reference to an array for emulating different
// access patterns. This test ensures that casting the reference to an array for
// processing works as expected.
TYPED_TEST(DramStateManagerTest, WorksWithArraysForBenchmarking) {
  const size_t kPerConnectionStateSize = 512;
  auto manager = this->CreateManager(kPerConnectionStateSize);
  unsigned int& state1 =
      manager->template GetStateForEvent<unsigned int>(this->dummy_event_);
  unsigned int* state1arr =
      reinterpret_cast<unsigned int*>(std::addressof(state1));
  for (int i = 0; i < kPerConnectionStateSize / sizeof(unsigned int); i++) {
    state1arr[i] = i;
  }

  unsigned int& state2 =
      manager->template GetStateForEvent<unsigned int>(this->dummy_event_);
  unsigned int* state2arr =
      reinterpret_cast<unsigned int*>(std::addressof(state2));
  CHECK_EQ(state1arr, state2arr);
  for (int i = 0; i < kPerConnectionStateSize / sizeof(unsigned int); i++) {
    CHECK_EQ(state1arr[i], state2arr[i]);
  }
}

// Tests for specific Dram State Managers
TEST(SingletonDramStateManagerTest, AlwaysReturnsSameState) {
  using EventT = falcon_rue::Event_GEN1;
  using ManagerT = SingletonDramStateManager<EventT>;
  struct PerConnectionStateT {
    int x;
  };

  auto manager = DramStateManagerTest<ManagerT>::CreateManager(
      sizeof(PerConnectionStateT));

  EventT event1{.connection_id = 1};
  auto& state1 =
      manager->template GetStateForEvent<PerConnectionStateT>(event1);
  std::vector<decltype(event1.connection_id)> connection_ids = {2, 5, 999};
  for (auto connection_id : connection_ids) {
    EventT event2{.connection_id = connection_id};
    auto& state2 =
        manager->template GetStateForEvent<PerConnectionStateT>(event2);
    EXPECT_EQ(std::addressof(state1), std::addressof(state2));
  }
}

TEST(ConnectionBitsDramStateManagerTest,
     ReturnsDifferentStateForDifferentConnections) {
  using EventT = falcon_rue::Event_GEN1;
  using ManagerT = ConnectionBitsDramStateManager<EventT>;
  struct PerConnectionStateT {
    int x;
  };

  auto manager = DramStateManagerTest<ManagerT>::CreateManager(
      sizeof(PerConnectionStateT));
  std::vector<decltype(EventT::connection_id)> connection_ids = {1, 2, 5, 999};
  for (auto connection_id : connection_ids) {
    EventT event{.connection_id = connection_id};
    auto& state =
        manager->template GetStateForEvent<PerConnectionStateT>(event);
    state.x = 100 + connection_id;
  }

  for (auto connection_id : connection_ids) {
    EventT event{.connection_id = connection_id};
    auto& state =
        manager->template GetStateForEvent<PerConnectionStateT>(event);
    EXPECT_EQ(state.x, 100 + connection_id);
  }
}

TEST(HashMapDramStateManagerTest,
     ReturnsDifferentStateForDifferentConnections) {
  using EventT = falcon_rue::Event_GEN1;
  using ManagerT = HashMapDramStateManager<EventT>;
  struct PerConnectionStateT {
    int x;
  };

  auto manager = DramStateManagerTest<ManagerT>::CreateManager(
      sizeof(PerConnectionStateT));
  std::vector<decltype(EventT::connection_id)> connection_ids = {1, 2, 5, 10};
  for (auto connection_id : connection_ids) {
    EventT event{.connection_id = connection_id};
    auto& state =
        manager->template GetStateForEvent<PerConnectionStateT>(event);
    state.x = 100 + connection_id;
  }

  for (auto connection_id : connection_ids) {
    EventT event{.connection_id = connection_id};
    auto& state =
        manager->template GetStateForEvent<PerConnectionStateT>(event);
    EXPECT_EQ(state.x, 100 + connection_id);
  }
}

TEST(HashMapDramStateManagerWithOnDemandInitTest,
     DefaultStateAndSavesStateForDifferentConnections) {
  using EventT = falcon_rue::Event_Gen2;
  using ManagerT = HashMapDramStateManagerWithOnDemandInit<EventT>;
  struct PerConnectionStateT {
    int x = 5;
  };

  auto default_state = PerConnectionStateT{};
  auto manager =
      std::make_unique<ManagerT>(sizeof(PerConnectionStateT), &default_state);
  std::vector<decltype(EventT::connection_id)> connection_ids = {1, 2, 5, 10};
  for (const auto& connection_id : connection_ids) {
    EventT event{.connection_id = connection_id};
    auto& state =
        manager->template GetStateForEvent<PerConnectionStateT>(event);
    // Expect state to be equal to the passed in default state.
    EXPECT_EQ(state.x, default_state.x);
    // Change the state for the connection.
    state.x = 100 + connection_id;
  }

  for (const auto& connection_id : connection_ids) {
    EventT event{.connection_id = connection_id};
    auto& state =
        manager->template GetStateForEvent<PerConnectionStateT>(event);
    // Expect state to be equal to the new state assigned above.
    EXPECT_EQ(state.x, 100 + connection_id);
  }
}

// Tests that the ConnectionBitsDramStateManager class constructor with the
// state default value is correctly initializing the state for all connections
// with the passed in default value.
TEST(ConnectionBitsDramStateManagerTest, InitializesDefaultValueCorrectly) {
  using EventT = falcon_rue::Event_Gen2;
  using ManagerT = ConnectionBitsDramStateManager<EventT>;
  struct PerConnectionStateT {
    int x = 2;  // default value of 2.
  };

  const uint8_t kMaxConnectionBits = 3;

  const auto kDefaultStateValue = PerConnectionStateT{};
  auto manager = std::make_unique<ManagerT>(
      sizeof(PerConnectionStateT), &kDefaultStateValue, kMaxConnectionBits);

  const size_t kMaxConnections = 1 << kMaxConnectionBits;
  for (decltype(EventT::connection_id) connection_id = 0;
       connection_id < kMaxConnections; connection_id++) {
    EventT event{.connection_id = connection_id};
    auto& state =
        manager->template GetStateForEvent<PerConnectionStateT>(event);
    EXPECT_EQ(state.x, kDefaultStateValue.x);
  }
}

TEST(TableWithOffsetInEventDramStateManagerTest,
     ReturnsDifferentStateForDifferentConnections) {
  using EventT = falcon_rue::Event_GEN1;
  using ManagerT = TableWithOffsetInEventDramStateManager<EventT>;
  struct PerConnectionStateT {
    int x;
  };

  auto manager = DramStateManagerTest<ManagerT>::CreateManager(
      sizeof(PerConnectionStateT));

  for (decltype(EventT::base_delay) i = 0; i < 10; i++) {
    EventT event{.base_delay = i};
    auto& state =
        manager->template GetStateForEvent<PerConnectionStateT>(event);
    state.x = 100 + i;
  }

  for (decltype(EventT::base_delay) i = 0; i < 10; i++) {
    EventT event{.base_delay = i};
    auto& state =
        manager->template GetStateForEvent<PerConnectionStateT>(event);
    EXPECT_EQ(state.x, 100 + i);
  }
}

TEST(TableWithOffsetInEventDramStateManagerTest, UpdatesBaseDelayCorrectly) {
  using EventT = falcon_rue::Event_GEN1;
  using ManagerT = TableWithOffsetInEventDramStateManager<EventT>;

  EventT event{.base_delay = 0};
  ManagerT::UpdateEventWithOffset(event, 5);
  EXPECT_EQ(event.base_delay, 5);

  // ensure we don't touch the top 4 bits.
  const size_t kMaxOffset = (1 << 20) - 1;
  event.base_delay = (0xC << 20);
  ManagerT::UpdateEventWithOffset(event, kMaxOffset);
  EXPECT_EQ((event.base_delay >> 20), 0xC);
  EXPECT_EQ((event.base_delay & kBaseDelayMask), kMaxOffset);
}

TEST(DramStateManagerHasOwnerTest, DetectsOwnerCorrectly) {
  using EventT = falcon_rue::Event_GEN1;
  using ManagerT = TableWithOffsetInEventDramStateManager<EventT>;
  using OwnedManagerT = MarkDramStateManagerAsHavingOwner<ManagerT>;
  EXPECT_FALSE(DramStateManagerHasOwner<ManagerT>::value);
  EXPECT_TRUE(DramStateManagerHasOwner<OwnedManagerT>::value);
}

}  // namespace
}  // namespace rue
}  // namespace isekai
