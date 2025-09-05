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

#ifndef ISEKAI_COMMON_STATUS_UTIL_H_
#define ISEKAI_COMMON_STATUS_UTIL_H_

#include <memory>
#include <sstream>
#include <string>

#include "absl/base/optimization.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "glog/logging.h"

#define STATUS_MACROS_IMPL_ELSE_BLOCKER_ \
  switch (0)                             \
  case 0:                                \
  default:

#define STATUS_MACROS_CONCAT_NAME(x, y) STATUS_MACROS_CONCAT_IMPL(x, y)
#define STATUS_MACROS_CONCAT_IMPL(x, y) x##y

// Crash the program if expr status is error.
#ifndef CHECK_OK
#define CHECK_OK(expr) \
  CHECK_OK_IMPL(STATUS_MACROS_CONCAT_NAME(_status, __COUNTER__), expr)

#define CHECK_OK_IMPL(status, expr) \
  if (auto status = expr; !status.ok()) LOG(FATAL) << status
#endif

// Crash the program if rexpr status is error, otherwise assign rexpr value to
// lhs.
#ifndef CHECK_OK_THEN_ASSIGN
#define CHECK_OK_THEN_ASSIGN(lhs, rexpr) \
  CHECK_OK_THEN_ASSIGN_IMPL(             \
      STATUS_MACROS_CONCAT_NAME(_status_or_value, __COUNTER__), lhs, rexpr)

#define CHECK_OK_THEN_ASSIGN_IMPL(statusor, lhs, rexpr) \
  auto statusor = (rexpr);                              \
  CHECK(statusor.status().ok()) << statusor.status();   \
  lhs = std::move(statusor).value()
#endif

// If expr status is error, then propagate the error status.
#ifndef RETURN_IF_ERROR
#define RETURN_IF_ERROR(expr)                         \
  STATUS_MACROS_IMPL_ELSE_BLOCKER_                    \
  if (isekai::status_macro_internal::StatusBuilder    \
          status_macro_internal_builder = {(expr)}) { \
  } else                                              \
    return status_macro_internal_builder
#endif

// If rexpr status is error, propagate the error status. Otherwise, assign rexpr
// value to lhs.
#ifndef ASSIGN_OR_RETURN
#define ASSIGN_OR_RETURN(...)                                          \
  GET_VARIADIC_MACRO(                                                  \
      (__VA_ARGS__, ASSIGN_OR_RETURN_3_IMPL, ASSIGN_OR_RETURN_2_IMPL)) \
  (__VA_ARGS__)

// This is how we overload macro based on the number of args.
#define GET_VARIADIC_MACRO(args) GET_VARIADIC_MACRO_HELPER args
#define GET_VARIADIC_MACRO_HELPER(_1, _2, _3, NAME, ...) NAME

#define ASSIGN_OR_RETURN_2_IMPL(lhs, rexpr) \
  ASSIGN_OR_RETURN_3_IMPL(lhs, rexpr, std::move(_))

// The error_expression will overwrite the non-OK status of rexpr.
#define ASSIGN_OR_RETURN_3_IMPL(lhs, rexpr, error_expression)               \
  ASSIGN_OR_RETURN_IMPL(                                                    \
      STATUS_MACROS_CONCAT_NAME(_status_or_value, __COUNTER__), lhs, rexpr, \
      error_expression)

#define ASSIGN_OR_RETURN_IMPL(statusor, lhs, rexpr, error_expression) \
  auto statusor = (rexpr);                                            \
  if (ABSL_PREDICT_FALSE(!statusor.ok())) {                           \
    isekai::status_macro_internal::StatusBuilder _(                   \
        std::move(statusor).status());                                \
    (void)_;                                                          \
    return (error_expression);                                        \
  }                                                                   \
  lhs = std::move(statusor).value()
#endif

namespace isekai {
namespace status_macro_internal {

class StatusBuilder {
 public:
  StatusBuilder(const StatusBuilder& status_builder) {
    status_ = status_builder.status_;
    stream_ =
        std::make_unique<std::ostringstream>(status_builder.stream_->str());
  }

  StatusBuilder(const absl::Status& orig_status)
      : status_(orig_status), stream_(new std::ostringstream) {}

  template <typename T>
  StatusBuilder& operator<<(const T& msg) {
    if (status_.ok()) return *this;
    *stream_ << msg << "; ";
    return *this;
  }

  absl::Status WrapStatus() {
    return absl::Status(status_.code(),
                        absl::StrCat(status_.message(), "; ", stream_->str()));
  }

  explicit operator bool() const { return ABSL_PREDICT_TRUE(status_.ok()); }

  operator absl::Status() const& {
    if (stream_->str().empty()) {
      return status_;
    }
    return StatusBuilder(*this).WrapStatus();
  }

  operator absl::Status() && {
    if (stream_->str().empty()) {
      return status_;
    }
    return WrapStatus();
  }

  absl::Status LogError() {
    if (!status_.ok()) {
      LOG(ERROR) << status_;
    }
    return *this;
  }

 private:
  absl::Status status_;
  std::unique_ptr<std::ostringstream> stream_;
};

void PrintFailure(absl::string_view expression, const absl::Status& status);

}  // namespace status_macro_internal
}  // namespace isekai

#endif  // ISEKAI_COMMON_STATUS_UTIL_H_
