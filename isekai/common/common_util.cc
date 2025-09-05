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

#include "isekai/common/common_util.h"

#include <string>

#include "absl/strings/string_view.h"
#include "glog/logging.h"
#include "isekai/common/config.pb.h"

namespace isekai {

HostConfigProfile GetHostConfigProfile(absl::string_view host_id,
                                       const NetworkConfig& network) {
  for (auto const& host_config : network.hosts()) {
    if (host_id == host_config.id()) {
      switch (host_config.config_case()) {
        case HostConfig::ConfigCase::kHostConfig: {
          return host_config.host_config();
        }
        case HostConfig::ConfigCase::kHostConfigProfileName: {
          // Gets the host config according to the profile name
          for (const auto& profile : network.host_configs()) {
            if (profile.profile_name() ==
                host_config.host_config_profile_name()) {
              return profile;
            }
          }
          LOG(FATAL) << "no profile is found: "
                     << host_config.host_config_profile_name();
        }
        case HostConfig::ConfigCase::CONFIG_NOT_SET: {
          LOG(INFO) << "host: " << host_id << " uses the default config.";
          return {};
        }
      }
    }
  }
  LOG(FATAL) << "host does not exist: " << host_id;
}

}  // namespace isekai
