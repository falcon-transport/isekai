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

#ifndef ISEKAI_HOST_FALCON_RUE_ALGORITHM_DRAM_STATE_MANAGER_H_
#define ISEKAI_HOST_FALCON_RUE_ALGORITHM_DRAM_STATE_MANAGER_H_

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "isekai/host/falcon/rue/bits.h"

namespace isekai {
namespace rue {

// This header defines different DRAM State Managers that can be used to manage
// connection state.

// Helper class to mark a dram state manager as having an owner.
template <class DramStateManager>
class MarkDramStateManagerAsHavingOwner : public DramStateManager {
 public:
  static constexpr bool kHasOwner = true;
  template <typename... Args>
  explicit MarkDramStateManagerAsHavingOwner(Args&&... args)
      : DramStateManager(std::forward<Args>(args)...) {}
};

// Compile-time helper to determine if a dram state manager has an owner.
// This is used by stateful algorithm to decide whether to accept the dram state
// manager as a unique pointer or a regular pointer.
template <typename T>
class DramStateManagerHasOwner {
 private:
  // If the given class has a member kHasOwner, this check function
  // will be prinused and return the boolean value.
  template <typename DramStateManager>
  static constexpr auto check(int)
      -> std::bool_constant<DramStateManager::kHasOwner>;

  // If the given manager does not have a kHasOwner member, this check function
  // will be used and return false.
  template <typename>
  static constexpr std::false_type check(...);

 public:
  static constexpr bool value = decltype(check<T>(0))::value;
};

// The `StatefulAlgorithm` expects all managers to have the function `StateT&
// GetStateForEvent(const EventT& event)`. While the eventual finalized code
// might have a fixed structure for the State, the benchmarking code varies the
// size of the state at runtime depending on command line arguments. To support
// both use cases, DRAM State Managers take a runtime dependent size variable
// (per connection state size) and return a `void*` to the state which can then
// be type casted to either an array (for benchmarking logic) or to the per
// connection state struct. To avoid repetition of logic, all managers inherit
// from `DramStateManagerInterface` and define a public method `void*
// GetRawStateForEvent(const EventT& event)`. Note that inheritance is done
// using CRTP to avoid performance penalties of virtual functions.
template <class Derived, typename EventT>
class DramStateManagerInterface {
 public:
  template <typename StateT>
  StateT& GetStateForEvent(const EventT& event) {
    auto& manager = static_cast<Derived&>(*this);
    void* ptr = manager.GetRawStateForEvent(event);
    return *reinterpret_cast<StateT*>(ptr);
  }
};

// As the SLC is sharded, every memory access of `kSlcSliceBoundary` may go to a
// different SLC. On the current IPUs, they are hashed to different SLCs at a
// 64B granularity.
const size_t kSlcSliceBoundary = 64;

// We plan on adding data structures with contiguous containers. The internal
// buffers of such containers may not necessarily align to an SLC boundary for
// common data types. A memory access in [0,64) is guaranteed to go
// to the same SLC (say X). Memory access to [64, 128) may or may not go to the
// same SLC X depending on the hashing function. If we do not start at a 64B
// boundary, and access [32, 96), we may end up accessing two different SLCs -
// [0, 64) and [64, 128). `SlcSlice` is a convenience structure to be used as a
// type for such containers so they can perform the necessary alignment.
struct alignas(kSlcSliceBoundary) SlcSlice {
  char data[kSlcSliceBoundary];
};

// Some data structures (e.g., TableWithOffsetInEventDramStateManager and
// soon-to-be-added VirtualAddressInEventDramStateManager) need to embed offsets
// or addresses in event fields. The number of bits and their masks are defined
// below.

// We repurpose the lower 20 bits of the event base_delay field.
inline constexpr uint8_t kBaseDelayFreeBitsLower = 20;
inline constexpr uint64_t kBaseDelayMask = (1 << kBaseDelayFreeBitsLower) - 1;

// SingletonDramStateManager always returns a reference to the same
// state regardless of the connection id. While not meant for production, it
// serves as an example of a data structure that can be used with
// StatefulAlgorithm. Additionally, it provides us with a lower bound of state
// access with the highest level of DRAM state locality achievable.
template <typename EventT>
class SingletonDramStateManager
    : public DramStateManagerInterface<SingletonDramStateManager<EventT>,
                                       EventT> {
 public:
  using EventType = EventT;
  SingletonDramStateManager() = delete;
  explicit SingletonDramStateManager(size_t state_size);
  void* GetRawStateForEvent(const EventT& event);

 private:
  // While the size of the state is varied, we would like it to be aligned to
  // the same granularity the SLC uses (kSlcSliceBoundary).
  // We allocate enough memory to store the singleton through a vector of type
  // SlcSlice `state_`, giving us contiguous memory as well as alignment to
  // SlcSlice.
  std::vector<SlcSlice> state_;
  // `state_buf_` will point to the internal buffer held by `state_` once we
  // reserve the required amount of space. We want to avoid having to request
  // the internal buffer on every call, as it does not change unless you resize
  // the vector (which we don't do once we've initially reserved the required
  // amount).
  char* state_buf_{nullptr};
};

template <typename EventT>
SingletonDramStateManager<EventT>::SingletonDramStateManager(
    size_t state_size) {
  CHECK_GT(state_size, 0);  // Crash OK

  // The state size is the total bytes for the state. We always allocate in
  // multiples of SLC slices though.
  size_t num_slices = std::ceil(static_cast<float>(state_size) /
                                static_cast<float>(sizeof(SlcSlice)));
  state_.resize(num_slices);
  state_buf_ = reinterpret_cast<char*>(state_.data());
}

template <typename EventT>
void* SingletonDramStateManager<EventT>::GetRawStateForEvent(
    const EventT& event) {
  return state_buf_;
}

// ConnectionBitKeyDramStateManager allocates a large amount of contiguous
// memory for the entire connection id space (24 bits). The offset into this
// table is the connection id itself.
template <typename EventT>
class ConnectionBitsDramStateManager
    : public DramStateManagerInterface<ConnectionBitsDramStateManager<EventT>,
                                       EventT> {
 public:
  using EventType = EventT;
  explicit ConnectionBitsDramStateManager(
      size_t state_size,
      size_t num_bits_in_connection_id = falcon_rue::kConnectionIdBits);
  // Initializes all table entries with the passed in default_state_value.
  explicit ConnectionBitsDramStateManager(
      size_t state_size, const void* default_state_value,
      size_t num_bits_in_connection_id = falcon_rue::kConnectionIdBits);
  void* GetRawStateForEvent(const EventT& event);

 private:
  // The per connection state size.
  const size_t state_size_;
  // A bitwise AND with `connection_id_mask_` ensures we are within bounds.
  decltype(EventT::connection_id) connection_id_mask_;
  // We store state for all connections in `table_` which is represented as a
  // vector of type `SlcSlice` for alignment.
  std::vector<SlcSlice> table_;
  // As `state_size_` may be smaller than an `SlcSlice` we find the right offset
  // through `mem_base_` which points to the data buffer in `table_` once space
  // has been reserved.
  char* mem_base_{nullptr};
};

template <typename EventT>
ConnectionBitsDramStateManager<EventT>::ConnectionBitsDramStateManager(
    size_t state_size, size_t num_bits_in_connection_id)
    : state_size_(state_size) {
  CHECK_GT(state_size_, 0);                // Crash OK
  CHECK_GT(num_bits_in_connection_id, 0);  // Crash OK
  size_t num_connections = 1 << num_bits_in_connection_id;
  size_t total_bytes = state_size_ * num_connections;
  size_t num_table_entries = std::ceil(static_cast<float>(total_bytes) /
                                       static_cast<float>(sizeof(SlcSlice)));
  connection_id_mask_ = num_connections - 1;

  table_.resize(num_table_entries);
  mem_base_ = reinterpret_cast<char*>(table_.data());
}

// Initializes all table entries with the passed in default_state_value.
template <typename EventT>
ConnectionBitsDramStateManager<EventT>::ConnectionBitsDramStateManager(
    size_t state_size, const void* default_state_value,
    size_t num_bits_in_connection_id)
    : ConnectionBitsDramStateManager(state_size, num_bits_in_connection_id) {
  size_t num_connections = 1 << num_bits_in_connection_id;
  for (size_t entry_idx = 0; entry_idx < num_connections; entry_idx++) {
    memcpy(mem_base_ + (entry_idx * state_size), default_state_value,
           state_size);
  }
}

template <typename EventT>
void* ConnectionBitsDramStateManager<EventT>::GetRawStateForEvent(
    const EventT& event) {
  size_t entry_idx = (event.connection_id & connection_id_mask_);
  char* state = mem_base_ + (entry_idx * state_size_);
  return state;
}

// TableWithOffsetInEventDramStateManager allocates just enough contiguous space
// required to support the maximum number of connections (`max_connections`).
// This is not the same as `ConnectionBitsDramStateManager` which allocates for
// the entire space representable by the bits used for a connection id. In
// practice, the NICs can only support a certain number of connections at a
// time and so we can get away with just enough table entries to support those
// connections. The idea is that when a connection is setup, an offset will be
// assigned to it. That same offset can be reused once the connection is
// terminated. Offset assignment is not in the critical path and not a
// part of this data structure. We assume that the offset is already present in
// the event. Currently, we repurpose unused bits from the base_delay field.
template <typename EventT>
class TableWithOffsetInEventDramStateManager
    : public DramStateManagerInterface<
          TableWithOffsetInEventDramStateManager<EventT>, EventT> {
 public:
  using EventType = EventT;
  explicit TableWithOffsetInEventDramStateManager(size_t state_size,
                                                  size_t max_connections);
  void* GetRawStateForEvent(const EventT& event);
  // Helper function to update the `event` fields with a given `offset`.
  static void UpdateEventWithOffset(EventT& event, size_t offset);

 private:
  const size_t state_size_;
  const size_t max_connections_;
  std::vector<SlcSlice> table_;
  char* mem_base_{nullptr};
};

template <typename EventT>
TableWithOffsetInEventDramStateManager<
    EventT>::TableWithOffsetInEventDramStateManager(size_t state_size,
                                                    size_t max_connections)
    : state_size_(state_size), max_connections_(max_connections) {
  CHECK_GT(state_size_, 0);       // Crash OK
  CHECK_GT(max_connections_, 0);  // Crash OK
  // As we only use the base_delay field for offsets, connections are limited by
  // the maximum representable value.
  CHECK_LT(max_connections_, 1 << kBaseDelayFreeBitsLower);  // Crash OK
  size_t total_bytes = state_size_ * max_connections_;
  size_t num_table_entries = std::ceil(static_cast<float>(total_bytes) /
                                       static_cast<float>(sizeof(SlcSlice)));
  table_.resize(num_table_entries);
  mem_base_ = reinterpret_cast<char*>(table_.data());
}

template <typename EventT>
void* TableWithOffsetInEventDramStateManager<EventT>::GetRawStateForEvent(
    const EventT& event) {
  size_t entry_idx = (event.base_delay & kBaseDelayMask);
  char* state = mem_base_ + (entry_idx * state_size_);
  return state;
}

template <typename EventT>
void TableWithOffsetInEventDramStateManager<EventT>::UpdateEventWithOffset(
    EventT& event, size_t offset) {
  // Ensure we are only using the bits we're supposed to be using
  decltype(event.base_delay) safe_offset = offset & kBaseDelayMask;
  event.base_delay = (event.base_delay & ~kBaseDelayMask) | safe_offset;
}

// HashMapDramStateManager looks up data using an absl flat hash map container
// with the connection id as the key. As this is for benchmarking, the state
// size is not known in advance. Therefore, the `max_connections` parameter is
// required to insert that many connections and resize the internal state to be
// able to hold `state_size` bytes per connection.
template <typename EventT>
class HashMapDramStateManager
    : public DramStateManagerInterface<HashMapDramStateManager<EventT>,
                                       EventT> {
 public:
  using EventType = EventT;
  explicit HashMapDramStateManager(size_t state_size, size_t max_connections);
  // Initializes all table entries with the passed in default_state_value.
  explicit HashMapDramStateManager(size_t state_size,
                                   const void* default_state_value);
  void* GetRawStateForEvent(const EventT& event);

 private:
  // For benchmarking, the state parameter/size is not known at compile time,
  // leaving us with a variable sized buffer for the value parameter. Future
  // DRAM state managers that operate on a fixed compile-time state size
  // (without any variable state size benchmarking) should directly use the
  // intended state type as the template value parameter.
  absl::flat_hash_map<uint32_t, std::vector<char>> state_;
  std::vector<char> default_state_;
};

template <typename EventT>
HashMapDramStateManager<EventT>::HashMapDramStateManager(
    size_t state_size, size_t max_connections) {
  for (size_t i = 0; i < max_connections; ++i) {
    state_[i].resize(state_size);
  }
}

template <typename EventT>
void* HashMapDramStateManager<EventT>::GetRawStateForEvent(
    const EventT& event) {
  auto& entry = state_[event.connection_id];
  return entry.data();
}

// HashMapDramStateManagerWithOnDemandInit looks up data using an absl flat hash
// map container with the connection id as the key. It is currently only used
// for Isekai and cannot be used for benchmarking because state would be
// allocated when retrieving the state for an uninitialized connection, which
// can affect benchmarking results. It initializes all future map entries with
// the passed in default_state_value.
template <typename EventT>
class HashMapDramStateManagerWithOnDemandInit
    : public DramStateManagerInterface<
          HashMapDramStateManagerWithOnDemandInit<EventT>, EventT> {
 public:
  using EventType = EventT;
  // Initializes all future map entries with the passed in default_state_value.
  explicit HashMapDramStateManagerWithOnDemandInit(
      size_t state_size, const void* default_state_value);
  void* GetRawStateForEvent(const EventT& event);

 private:
  absl::flat_hash_map<uint32_t, std::vector<char>> state_;
  std::vector<char> default_state_;
};

template <typename EventT>
HashMapDramStateManagerWithOnDemandInit<EventT>::
    HashMapDramStateManagerWithOnDemandInit(size_t state_size,
                                            const void* default_state_value) {
  default_state_.resize(state_size);
  memcpy(default_state_.data(), default_state_value, state_size);
}

template <typename EventT>
void* HashMapDramStateManagerWithOnDemandInit<EventT>::GetRawStateForEvent(
    const EventT& event) {
  // If connection ID not in the map, initialize with the default state.
  if (state_.find(event.connection_id) == state_.end()) {
    auto& entry = state_[event.connection_id];
    entry.resize(default_state_.size());
    entry = default_state_;
  }
  return (state_[event.connection_id]).data();
}

}  // namespace rue
}  // namespace isekai

#endif  // ISEKAI_HOST_FALCON_RUE_ALGORITHM_DRAM_STATE_MANAGER_H_
