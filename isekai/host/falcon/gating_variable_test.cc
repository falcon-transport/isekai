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

#include "isekai/host/falcon/gating_variable.h"

#include <utility>

#include "gtest/gtest.h"

namespace isekai {
namespace {

template <typename T>
class GatingVariableTest : public ::testing::Test {};

typedef ::testing::Types<int, uint32_t, uint64_t, double> SupportedTypes;

TYPED_TEST_SUITE(GatingVariableTest, SupportedTypes);

// Tests if the GatingVariable is initialized with correct values.
TYPED_TEST(GatingVariableTest, Initialization) {
  GatingFunction f = [] {};
  GatingVariable<TypeParam> x1(f);
  EXPECT_EQ(x1, 0);
  GatingVariable<TypeParam> x2(42);
  EXPECT_EQ(x2, 42);
  GatingVariable<TypeParam> x3(f, 42);
  EXPECT_EQ(x3, 42);
}

// Tests if the GatingVariable calls the GatingFunction with operator=.
TYPED_TEST(GatingVariableTest, Assignment) {
  bool is_called = false;
  GatingFunction f = [&is_called] { is_called = true; };

  // Check if gating function is called upon value update via assignment.
  GatingVariable<TypeParam> x(std::move(f));
  x = 42;
  EXPECT_TRUE(is_called);
  EXPECT_EQ(x, 42);

  // Check if gating variable can be assigned to primitive type via assignment.
  TypeParam y = x;
  EXPECT_EQ(y, 42);
}

TYPED_TEST(GatingVariableTest, OperateAndAssignment) {
  int call_count = 0;
  GatingFunction f = [&call_count] { ++call_count; };

  GatingVariable<TypeParam> x(std::move(f), 42);
  x += 1;
  EXPECT_EQ(call_count, 1);
  EXPECT_EQ(x, 43);

  x -= 1;
  EXPECT_EQ(call_count, 2);
  EXPECT_EQ(x, 42);

  x *= 2;
  EXPECT_EQ(call_count, 3);
  EXPECT_EQ(x, 84);

  x /= 3;
  EXPECT_EQ(call_count, 4);
  EXPECT_EQ(x, 28);

  TypeParam t;
  t = ++x;
  EXPECT_EQ(call_count, 5);
  EXPECT_EQ(t, 29);

  t = x++;
  EXPECT_EQ(call_count, 6);
  EXPECT_EQ(t, 29);

  t = --x;
  EXPECT_EQ(call_count, 7);
  EXPECT_EQ(t, 29);

  t = x--;
  EXPECT_EQ(call_count, 8);
  EXPECT_EQ(t, 29);

  EXPECT_EQ(x, 28);
}

}  // namespace
}  // namespace isekai
