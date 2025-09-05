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

#ifndef ISEKAI_TESTING_INTEGRATION_OMNEST_EMBEDDED_KERNEL_H_
#define ISEKAI_TESTING_INTEGRATION_OMNEST_EMBEDDED_KERNEL_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/strings/str_cat.h"
#include "glog/logging.h"
#include "isekai/common/config.pb.h"
#include "isekai/common/file_util.h"
#include "omnetpp/ccomponent.h"
#include "omnetpp/cconfiguration.h"
#include "omnetpp/cenvir.h"
#include "omnetpp/cnullenvir.h"
#include "omnetpp/cownedobject.h"
#include "omnetpp/csimulation.h"
#include "omnetpp/cxmlelement.h"
#include "omnetpp/opp_string.h"
#include "omnetpp/simtime_t.h"
#include "xmldoccache.h"

namespace isekai {

constexpr char kTestDir[] = "isekai/testing/integration/";
constexpr char kInetDir[] = "external/inet/src/";
constexpr char kNetworkModelDir[] = "isekai/fabric";

// The basic environment that reads parameter values from NED and ini files.
class MinimalEnvironment : public omnetpp::cNullEnvir {
 public:
  MinimalEnvironment(int ac, char** av, omnetpp::cConfigurationEx* config)
      : cNullEnvir(/* ac = */ ac, /* av = */ av, /* c = */ config),
        config_(config) {
    xml_cache_ = new omnetpp::envir::XMLDocCache();
  }

  ~MinimalEnvironment() override { delete xml_cache_; }

  // Reads model parameters from NED or ini files.
  void readParameter(omnetpp::cPar* par) override;
  // Records scalar statistical results.
  void recordScalar(omnetpp::cComponent* component, const char* name,
                    double value,
                    omnetpp::opp_string_map* attributes) override {
    statistics_[absl::StrCat(component->getFullPath(), ".", name)] = value;
  }
  // Parses the XML parameter from ini file.
  omnetpp::cXMLElement* getXMLDocument(const char* filename,
                                       const char* xpath) override {
    omnetpp::cXMLElement* document_node = xml_cache_->getDocument(filename);
    return ResolveXmlPath(document_node, xpath);
  }
  omnetpp::cXMLElement* getParsedXMLString(const char* content,
                                           const char* xpath) override {
    LOG(INFO) << "getParsedXMLString";
    omnetpp::cXMLElement* document_node = xml_cache_->getParsed(content);
    return ResolveXmlPath(document_node, xpath);
  }

  const absl::flat_hash_map<std::string, double>& get_statistics() const {
    return statistics_;
  }

 private:
  omnetpp::cXMLElement* ResolveXmlPath(omnetpp::cXMLElement* document_node,
                                       const char* path) {
    if (path) {
      omnetpp::ModNameParamResolver resolver(
          omnetpp::getSimulation()->getContextModule());
      return omnetpp::cXMLElement::getDocumentElementByPath(document_node, path,
                                                            &resolver);
    } else {
      // returns the root element (child of the document node)
      return document_node->getFirstChild();
    }
  }

  omnetpp::cConfigurationEx* const config_;
  // Stores all scalar statistical results.
  absl::flat_hash_map<std::string, double> statistics_;
  // Used to parse the xml configuration.
  omnetpp::envir::XMLDocCache* xml_cache_ = nullptr;
};

class OmnestEmbeddedKernel {
 public:
  // Initializes the OMNest kernel.
  OmnestEmbeddedKernel(const char* ned_file_dir, const char* ini_file,
                       const char* network_name,
                       double simulation_time) noexcept;

  // Cleans up before exiting simulation.
  // By default, the destructor is marked as noexcept.
  ~OmnestEmbeddedKernel();
  // Runs the simulation.
  void ExecuteSimulation() noexcept;
  // Gets the collected statistics.
  double GetStatistics(std::string key) const;

  // Gets a copy of the SimulationConfig for the integration test.
  SimulationConfig GetSimulationConfig() const;

 private:
  // Loads the NED files in the directory.
  void LoadNedFiles(const char* dir);
  // Parse the configuration from ini file.
  void ParseOmnestConfiguration(const char* ini_file);

  // The embedded simulation kernel.
  std::unique_ptr<omnetpp::cSimulation> embed_sim_;
  // The simulation environment, "owned" by OMNest.
  omnetpp::cEnvir* sim_env_;
  // The simulation configuration, "owned" by OMNest.
  omnetpp::cConfigurationEx* sim_config_;
  // Declares to let embedded simulation kernel act running in Main().
  const omnetpp::cStaticFlag dummy_;
};

};  // namespace isekai

#endif  // ISEKAI_TESTING_INTEGRATION_OMNEST_EMBEDDED_KERNEL_H_
