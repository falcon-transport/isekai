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

#ifndef ISEKAI_OPEN_SOURCE_DEFAULT_FILE_UTIL_H_
#define ISEKAI_OPEN_SOURCE_DEFAULT_FILE_UTIL_H_

#include <fcntl.h>

#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "glog/logging.h"
#include "riegeli/bytes/fd_reader.h"
#include "riegeli/bytes/fd_writer.h"
#include "riegeli/messages/text_parse_message.h"
#include "riegeli/records/record_reader.h"
#include "riegeli/records/record_writer.h"

namespace isekai {

// Reads proto from RecordIO file.
template <typename T>
void ReadProtoFromRecordioFile(absl::string_view input_proto_file, T& proto) {
  riegeli::RecordReader reader{riegeli::FdReader(input_proto_file)};
  CHECK(reader.ReadRecord(proto)) << reader.status();
  CHECK(reader.Close()) << reader.status();
}

// Reads proto from text proto file.
template <typename T>
absl::Status ReadTextProtoFromFile(absl::string_view input_text_proto_file,
                                   T* proto) {
  CHECK_EQ(riegeli::TextParseMessage(riegeli::FdReader(input_text_proto_file),
                                     *proto),
           absl::OkStatus());
  return absl::OkStatus();
}

// Writes proto to recordIO file.
template <typename T>
absl::Status WriteProtoToFile(absl::string_view output_file_path,
                              const T& proto) {
  riegeli::RecordWriter writer{riegeli::FdWriter(output_file_path)};
  CHECK(writer.WriteRecord(proto)) << writer.status();
  CHECK(writer.Close()) << writer.status();
  return absl::OkStatus();
}

// Joins two file paths, e.g., path1 = "abc", while path2 = "def", the joined
// path is "abc/def".
std::string FileJoinPath(absl::string_view path1, absl::string_view path2);

// Will overwrite the file with the given file content. Return error if file
// does not exist.
absl::Status WriteStringToFile(const absl::string_view file_path,
                               const absl::string_view file_content);

}  // namespace isekai

#endif  // ISEKAI_OPEN_SOURCE_DEFAULT_FILE_UTIL_H_
