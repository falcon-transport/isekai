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

#include "isekai/fabric/routing_test_util.h"

#include <cstdint>

#include "absl/strings/string_view.h"
#include "isekai/fabric/routing.pb.h"

#undef ETHER_ADDR_LEN
#undef ETHER_TYPE_LEN

#include "google/protobuf/text_format.h"
#include "inet/common/INETMath.h"
#include "inet/common/packet/Packet.h"
#include "inet/linklayer/common/MacAddress.h"
#include "inet/linklayer/common/MacAddressTag_m.h"
#include "inet/networklayer/common/L3Tools.h"
#include "inet/networklayer/ipv6/Ipv6Header.h"
#include "isekai/common/status_util.h"
#include "omnetpp.h"
#include "omnetpp/checkandcast.h"
#include "omnetpp/cmessage.h"
#include "riegeli/bytes/fd_writer.h"
#include "riegeli/records/record_writer.h"
#undef NaN
#include "isekai/host/rnic/omnest_packet_builder.h"

namespace isekai {

absl::Status WriteTestRoutingConfigToFile(absl::string_view output_file) {
  riegeli::FdWriter file(output_file);
  CHECK(file.ok()) << "Fail to create file: " << file.status();
  riegeli::RecordWriter writer(std::move(file));
  CHECK(writer.ok()) << "Fail to initialize recordio writer: "
                     << writer.status();
  RoutingConfig proto;
  google::protobuf::TextFormat::ParseFromString(
      kTestRoutingConfigWithUpAndDownVrf, &proto);
  CHECK(writer.WriteRecord(proto))
      << "Fail to write proto: " << writer.status();
  CHECK(writer.Close());

  return absl::OkStatus();
}

void SetPacketSourceMacInd(const MacAddress& mac_address,
                           omnetpp::cMessage* packet) {
  auto inet_packet = omnetpp::check_and_cast<inet::Packet*>(packet);
  auto mac_address_req = inet_packet->addTagIfAbsent<inet::MacAddressInd>();
  mac_address_req->setSrcAddress(
      inet::MacAddress(mac_address.ToString().c_str()));
}

omnetpp::cSimulation* SetupDummyOmnestSimulation() {
  omnetpp::CodeFragments::executeAll(omnetpp::CodeFragments::STARTUP);
  omnetpp::SimTime::setScaleExp(-12);

  omnetpp::cEnvir* env = new omnetpp::cNullEnvir(0, nullptr, nullptr);
  omnetpp::cSimulation* sim = new omnetpp::cSimulation("simulation", env);
  omnetpp::cSimulation::setActiveSimulation(sim);
  sim->callInitialize();

  return sim;
}

void CloseOmnestSimulation(omnetpp::cSimulation* sim) {
  sim->callFinish();
  omnetpp::cSimulation::setActiveSimulation(nullptr);
  delete sim;
  omnetpp::CodeFragments::executeAll(omnetpp::CodeFragments::SHUTDOWN);
}

std::unique_ptr<inet::Packet> GenerateTestInetPacket(
    const std::string& src_ipv6_address, const std::string& dst_ipv6_address,
    const MacAddress& flow_src_mac, uint32_t src_port, uint32_t dest_port) {
  OmnestPacketBuilder packet_builder(
      /* host_module = */ nullptr,
      /* falcon = */ nullptr,
      /* env = */ nullptr,
      /* stats_collector = */ nullptr,
      /* ip_address = */ "", /* transmission_channel = */ nullptr,
      /* host_id = */ "");

  auto packet = packet_builder.CreatePacketWithUdpAndIpv6Headers(
      src_ipv6_address, dst_ipv6_address, src_port, dest_port);
  SetPacketSourceMacInd(flow_src_mac, packet.get());

  return packet;
}

}  // namespace isekai
