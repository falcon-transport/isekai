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

#ifndef ISEKAI_OPEN_SOURCE_DEFAULT_TESTING_H_
#define ISEKAI_OPEN_SOURCE_DEFAULT_TESTING_H_

#include <ostream>
#include <string>
#include <type_traits>
#include <utility>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/status_util.h"

namespace isekai::internal {

inline const absl::Status& GetStatus(const absl::Status& status) {
  return status;
}

template <typename T>
inline const absl::Status& GetStatus(const absl::StatusOr<T>& status) {
  return status.status();
}

// StatusIs() is a polymorphic matcher.  This class is the common
// implementation of it shared by all types T where StatusIs() can be
// used as a Matcher<T>.
class StatusIsMatcherCommonImpl {
 public:
  StatusIsMatcherCommonImpl(
      ::testing::Matcher<absl::StatusCode> code_matcher,
      ::testing::Matcher<const std::string&> message_matcher)
      : code_matcher_(std::move(code_matcher)),
        message_matcher_(std::move(message_matcher)) {}

  void DescribeTo(std::ostream* os) const {
    *os << ", has a status code that ";
    code_matcher_.DescribeTo(os);
    *os << ", and has an error message that ";
    message_matcher_.DescribeTo(os);
  }

  void DescribeNegationTo(std::ostream* os) const {
    *os << ", or has a status code that ";
    code_matcher_.DescribeNegationTo(os);
    *os << ", or has an error message that ";
    message_matcher_.DescribeNegationTo(os);
  }

  bool MatchAndExplain(const absl::Status& status,
                       ::testing::MatchResultListener* result_listener) const {
    ::testing::StringMatchResultListener inner_listener;

    inner_listener.Clear();
    if (!code_matcher_.MatchAndExplain(status.code(), &inner_listener)) {
      *result_listener << (inner_listener.str().empty()
                               ? "whose status code is wrong"
                               : "which has a status code " +
                                     inner_listener.str());
      return false;
    }

    if (!message_matcher_.Matches(std::string(status.message()))) {
      *result_listener << "whose error message is wrong";
      return false;
    }

    return true;
  }

 private:
  const ::testing::Matcher<absl::StatusCode> code_matcher_;
  const ::testing::Matcher<const std::string&> message_matcher_;
};

// Monomorphic implementation of matcher StatusIs() for a given type
// T.  T can be Status, StatusOr<>, or a reference to either of them.
template <typename T>
class MonoStatusIsMatcherImpl : public ::testing::MatcherInterface<T> {
 public:
  explicit MonoStatusIsMatcherImpl(StatusIsMatcherCommonImpl common_impl)
      : common_impl_(std::move(common_impl)) {}

  void DescribeTo(std::ostream* os) const override {
    common_impl_.DescribeTo(os);
  }

  void DescribeNegationTo(std::ostream* os) const override {
    common_impl_.DescribeNegationTo(os);
  }

  bool MatchAndExplain(
      T actual_value,
      ::testing::MatchResultListener* result_listener) const override {
    return common_impl_.MatchAndExplain(GetStatus(actual_value),
                                        result_listener);
  }

 private:
  StatusIsMatcherCommonImpl common_impl_;
};

// Implements StatusIs() as a polymorphic matcher.
class StatusIsMatcher {
 public:
  StatusIsMatcher(::testing::Matcher<absl::StatusCode> code_matcher,
                  ::testing::Matcher<const std::string&> message_matcher)
      : common_impl_(std::move(code_matcher), std::move(message_matcher)) {}

  // Converts this polymorphic matcher to a monomorphic matcher of the given
  // type.  T can be StatusOr<>, Status, or a reference to either of them.
  template <typename T>
  operator ::testing::Matcher<T>() const {
    return ::testing::MakeMatcher(new MonoStatusIsMatcherImpl<T>(common_impl_));
  }

 private:
  const StatusIsMatcherCommonImpl common_impl_;
};

// Monomorphic implementation of matcher IsOk() for a given type T.
// T can be Status, StatusOr<>, or a reference to either of them.
template <typename T>
class MonoIsOkMatcherImpl : public ::testing::MatcherInterface<T> {
 public:
  void DescribeTo(std::ostream* os) const override { *os << "is OK"; }
  void DescribeNegationTo(std::ostream* os) const override {
    *os << "is not OK";
  }
  bool MatchAndExplain(T actual_value,
                       ::testing::MatchResultListener*) const override {
    return GetStatus(actual_value).ok();
  }
};

// Implements IsOk() as a polymorphic matcher.
class IsOkMatcher {
 public:
  template <typename T>
  operator ::testing::Matcher<T>() const {
    return ::testing::MakeMatcher(new MonoIsOkMatcherImpl<T>());
  }
};

// Returns a gMock matcher that matches a Status or StatusOr<> whose status code
// matches code_matcher, and whose error message matches message_matcher.
template <typename StatusCodeMatcher>
StatusIsMatcher StatusIs(
    StatusCodeMatcher&& code_matcher,
    ::testing::Matcher<const std::string&> message_matcher) {
  return StatusIsMatcher(std::forward<StatusCodeMatcher>(code_matcher),
                         std::move(message_matcher));
}

// Returns a gMock matcher that matches a Status or StatusOr<> whose status code
// matches code_matcher.
template <typename StatusCodeMatcher>
StatusIsMatcher StatusIs(StatusCodeMatcher&& code_matcher) {
  return StatusIs(std::forward<StatusCodeMatcher>(code_matcher), ::testing::_);
}

// Returns a gMock matcher that matches a Status or StatusOr<> which is OK.
inline IsOkMatcher IsOk() { return IsOkMatcher(); }

}  // namespace isekai::internal

// Macros for testing the results of functions that return absl::Status or
// absl::StatusOr<T> (for any type T).
#ifndef EXPECT_OK
#define EXPECT_OK(expression) \
  EXPECT_THAT(expression, ::isekai::internal::IsOk())
#endif

#ifndef ASSERT_OK
#define ASSERT_OK(expr) ASSERT_THAT(expr, ::isekai::internal::IsOk())
#endif

#ifndef ASSERT_OK_AND_ASSIGN
#define ASSERT_OK_AND_ASSIGN(lhs, rexpr) \
  ASSIGN_OR_RETURN(lhs, rexpr,           \
                   isekai::status_macro_internal::PrintFailure(#rexpr, _))
#endif

#endif  // ISEKAI_OPEN_SOURCE_DEFAULT_TESTING_H_
