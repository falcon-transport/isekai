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

#include "isekai/common/file_util.h"

#include <fstream>
#include <sstream>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/testing.h"

namespace {

TEST(FileUtilTest, ReadTextProto) {
  isekai::SimulationConfig simulation_config;
  EXPECT_OK(ReadTextProtoFromFile("isekai/test_data/config.pb.txt",
                                  &simulation_config));

  EXPECT_EQ(simulation_config.network().hosts_size(), 2);
  EXPECT_EQ(simulation_config.network().hosts(0).id(), "host1");
  EXPECT_EQ(simulation_config.network().hosts(1).id(), "host2");
}

TEST(FileUtilTest, JoinTwoPaths) {
  const std::string joined_path = "abc/def";
  EXPECT_EQ(joined_path, isekai::FileJoinPath("abc", "def"));
  EXPECT_EQ(joined_path, isekai::FileJoinPath("abc/", "def"));
  EXPECT_EQ(joined_path, isekai::FileJoinPath("abc", "/def"));
  EXPECT_EQ(joined_path, isekai::FileJoinPath("abc/", "/def"));
}

TEST(FileUtilTest, WriteFile) {
  std::string file_content = "write file test string";
  std::string file_path =
      isekai::FileJoinPath(::testing::TempDir(), "test/write_test.txt");
  EXPECT_OK(isekai::WriteStringToFile(file_path, file_content));

  std::ifstream ifs(file_path);
  std::ostringstream sstr;
  sstr << ifs.rdbuf();
  EXPECT_EQ(file_content, sstr.str());
}

}  // namespace
