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

#include "isekai/testing/integration/omnest_embedded_kernel.h"

#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "glog/logging.h"
#include "inifilereader.h"
#include "isekai/common/file_util.h"
#include "isekai/host/rnic/omnest_environment.h"
#include "omnetpp.h"
#include "omnetpp/ccomponenttype.h"
#include "omnetpp/cconfiguration.h"
#include "omnetpp/cexception.h"
#include "omnetpp/csimulation.h"
#include "omnetpp/simtime.h"
#include "omnetpp/simtime_t.h"
#include "sectionbasedconfig.h"
#include "stringutil.h"

namespace isekai {

void MinimalEnvironment::readParameter(omnetpp::cPar* par) {
  CHECK(!par->isSet()) << "parameter is already set.";

  // Gets it from the ini file
  std::string moduleFullPath = par->getOwner()->getFullPath();
  const char* str = config_->getParameterValue(
      moduleFullPath.c_str(), par->getName(), par->containsValue());

  if (omnetpp::opp_strcmp(str, "default") == 0) {
    // Gets the default value from NED file first.
    CHECK(par->containsValue()) << "No default value given.";
    par->acceptDefault();
  } else if (!omnetpp::common::opp_isempty(str)) {
    // Gets the par value from ini file.
    par->parse(str);
  } else {
    CHECK(par->containsValue()) << "No value assigned to the parameter.";
    par->acceptDefault();
  }

  CHECK(par->isSet()) << "Fail to set parameter value.";
}

OmnestEmbeddedKernel::OmnestEmbeddedKernel(const char* ned_file_dir,
                                           const char* ini_file,
                                           const char* network_name,
                                           double simulation_time) noexcept {
  // Tricky: we must shutdown and startup so that the embedded simulation kernel
  // could run multiple times in unit testing.
  omnetpp::CodeFragments::executeAll(omnetpp::CodeFragments::SHUTDOWN);
  omnetpp::CodeFragments::executeAll(omnetpp::CodeFragments::STARTUP);
  // Simulation time resolution is ns.
  omnetpp::SimTime::setScaleExp(kOmnestTimeResolution);

  LoadNedFiles(ned_file_dir);
  omnetpp::cModuleType* network_type = omnetpp::cModuleType::find(network_name);
  CHECK(network_type != nullptr) << "No such network.";

  // Gets the ini configuration.
  ParseOmnestConfiguration(ini_file);

  // Provides the simulation env.
  sim_env_ = new MinimalEnvironment(/* ac = */ 0, /* av = */ nullptr,
                                    /* config = */ sim_config_);

  // Initializes embed_sim_ based on the sim_env;
  embed_sim_ = std::make_unique<omnetpp::cSimulation>("simulation", sim_env_);

  // Makes the embed simulation active.
  omnetpp::cSimulation::setActiveSimulation(embed_sim_.get());

  // Sets up the network and prepare to run it.
  try {
    embed_sim_->setupNetwork(network_type);
    embed_sim_->setSimulationTimeLimit(omnetpp::SimTime(simulation_time));
    embed_sim_->callInitialize();
  } catch (const std::exception& e) {
    LOG(FATAL) << "ERROR: " << e.what();
  }
}

OmnestEmbeddedKernel::~OmnestEmbeddedKernel() {
  if (embed_sim_ != nullptr) {
    embed_sim_->deleteNetwork();
    omnetpp::cSimulation::setActiveSimulation(nullptr);
  }

  // Do not double free the sim_env_ and sim_config_.
  // They have been already deleted by calling deleteNetwork().
  omnetpp::CodeFragments::executeAll(omnetpp::CodeFragments::SHUTDOWN);
}

void OmnestEmbeddedKernel::LoadNedFiles(const char* dir) {
  omnetpp::cSimulation::loadNedSourceFolder(dir);
  // Loads the NED file from INET and network model as well.
  omnetpp::cSimulation::loadNedSourceFolder(kInetDir);
  if (std::filesystem::exists(kNetworkModelDir)) {
    omnetpp::cSimulation::loadNedSourceFolder(kNetworkModelDir);
  }
  omnetpp::cSimulation::doneLoadingNedFiles();
}

SimulationConfig OmnestEmbeddedKernel::GetSimulationConfig() const {
  isekai::SimulationConfig simulation;
  std::string simulation_config_file =
      embed_sim_->getSystemModule()->par("simulation_config").stringValue();
  CHECK(ReadTextProtoFromFile(simulation_config_file, &simulation).ok());
  return simulation;
}

void OmnestEmbeddedKernel::ExecuteSimulation() noexcept {
  bool running = true;
  // Has to use try-catch here to catch simulation termination or possible
  // errors from OMNest.
  try {
    while (true) {
      omnetpp::cEvent* event = embed_sim_->takeNextEvent();
      // Stop running if no more events in the simulation.
      if (!event) {
        LOG(INFO) << "No events in the simulation. Stop now.";
        break;
      }
      // Executes the scheduled event.
      embed_sim_->executeEvent(event);
    }
  } catch (const omnetpp::cTerminationException& e) {
    LOG(INFO) << "Simulation time's up: " << e.what();
  } catch (const std::exception& e) {
    running = false;
    LOG(FATAL) << "Simulation ERROR: " << e.what();
  }

  if (running) embed_sim_->callFinish();
}

void OmnestEmbeddedKernel::ParseOmnestConfiguration(const char* ini_file) {
  auto bootConfig = new omnetpp::envir::SectionBasedConfiguration();
  auto inifile_reader = new omnetpp::envir::InifileReader();
  inifile_reader->readFile(absl::StrCat(kTestDir, ini_file).c_str());

  bootConfig->setConfigurationReader(inifile_reader);
  sim_config_ = bootConfig;
}

double OmnestEmbeddedKernel::GetStatistics(std::string key) const {
  auto stats = static_cast<MinimalEnvironment*>(sim_env_)->get_statistics();
  DCHECK(stats.find(key) != stats.end()) << "No such key: " << key;
  return stats[key];
}

}  // namespace isekai
