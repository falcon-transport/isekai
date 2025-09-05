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

#ifndef ISEKAI_FABRIC_MODEL_INTERFACES_H_
#define ISEKAI_FABRIC_MODEL_INTERFACES_H_

#include <cstdint>

#include "isekai/fabric/constants.h"
#include "isekai/fabric/pfc_message_m.h"
#include "omnetpp/cdataratechannel.h"
#include "omnetpp/cmessage.h"
#include "omnetpp/csimplemodule.h"
#include "omnetpp/simtime_t.h"

namespace isekai {

// The types of events occurred in the network router.
enum class RouterEventType : uint16_t {
  kReceivePacket = 0,
  kRouterDelay = 1,
  kOutputScheduling = 2,
  kSetPfcPaused = 3,
  kPfcRefreshment = 4,
  kPfcPauseSend = 5,
  kCollectPortStatistics = 7,
};

// The packet queue interface. Its subclasse are implemented to store the Tx
// packets or PFC messages.
class PacketQueueInterface {
 public:
  virtual ~PacketQueueInterface() {}
  // Returns true if successfully enqueues a packet, otherwise false.
  virtual bool Enqueue(omnetpp::cMessage* packet) = 0;
  // Returns nullptr if fails to dequeue a packet.
  virtual omnetpp::cMessage* Dequeue() = 0;
  // Returns the queue length.
  virtual size_t Size() = 0;
  // Returns the queue length in bytes.
  virtual size_t Bytes() = 0;
  // Peeks the next popped out packet in the queue. Returns nullptr if there has
  // none.
  virtual omnetpp::cMessage* Peek() = 0;
};

// An abstract base class whose subclasses provide various packet arbitration
// policies, e.g., fixed priority or weighted round-robin.
class ArbiterInterface {
 public:
  virtual ~ArbiterInterface() {}
  // Arbitrates a tx packet from the input queues. The size of packet_list
  // equals the number of input queues. The queues that are not eligible to
  // send will be represented with nullptr. Return -1 if no packet can be
  // dequeued from queues, otherwise the queue index of the dequeued packet.
  virtual int Arbitrate(
      const std::vector<const omnetpp::cMessage*>& packet_list) = 0;
};

// Network router port interface, which is a inout port.
class PortInterface {
 public:
  virtual ~PortInterface() {}
  // Router calls this function to handle the packet received from the network.
  virtual void ReceivePacket(omnetpp::cMessage* packet) = 0;
  // Router calls this function to enqueue the packet from routing pipeline.
  virtual void EnqueuePacket(omnetpp::cMessage* packet) = 0;
  // Router calls this function to send a packet in packet queues to
  // the network. The port will do nothing (1) if there has no packets to
  // send out (enter Idle state); (2) if the state is active, i.e., the port is
  // transmitting a packet to the network.
  virtual void ScheduleOutput() = 0;
  // MMU calls this function to generate PFC.
  virtual void GeneratePfc(int priority, int16_t pause_units) = 0;
  // MMU calls this function to get the information of packet queue.
  virtual PacketQueueInterface* GetPacketQueue(int priority) const = 0;
  virtual omnetpp::cDatarateChannel* GetTransmissionChannel() const = 0;
  // When PFC pause time is up, this function is called to resume packet
  // transmission of a certain port if possible.
  virtual void ResumeOutput() = 0;
  // Collect statistics at port.
  virtual void CollectPortStatistics() = 0;
};

// The routing pipeline provides the routing features of the router.
class RoutingPipelineInterface {
 public:
  virtual ~RoutingPipelineInterface() {}
  // Performs all routing steps to determine the output port of the packet, and
  // then forwards the packet to the corresponding output port.
  virtual void RoutePacket(omnetpp::cMessage* packet) = 0;
};

// Memory management unit (MMU) tracks the state of each packet queue and
// controls if enqueue and dequeue operations are allowed. It also triggers and
// reacts to control frames (e.g., PFC), and control-based packet modifications
// (e.g., ECN).
class MemoryManagementUnitInterface {
 public:
  virtual ~MemoryManagementUnitInterface() {}
  // Processes ingress counting for PFC feature. Return false to indicate packet
  // drop if not enough memory is left to handle the received packet.
  virtual IngressMemoryOccupancy ReceivedData(uint32_t port_id,
                                              uint64_t packet_size,
                                              int priority) = 0;
  // Reacts to control frames, setting state of the corresponding queue.
  virtual void ReceivedPfc(uint32_t port_id, omnetpp::cMessage* packet) = 0;
  // Returns true if the packet could be enqueued, otherwise false. Different
  // queueing policies like passive queueing (drop-tail) or active queueing
  // (WRED) could be implemented in subclasses. The actual implementation of
  // this function in the subclass may modify the packet, e.g., for support ECN
  // marking.
  virtual bool RequestEnqueue(uint32_t port_id, int priority,
                              omnetpp::cMessage* packet) = 0;
  // Returns false if the queue is empty or in pause state, otherwise true.
  virtual bool CanTransmit(uint32_t port_id, int priority) = 0;
  // Restores the status of the queue after dequeueing the packet for
  // transmission.
  virtual void DequeuedPacket(uint32_t port_id, int priority,
                              omnetpp::cMessage* packet) = 0;
  // Updates the pause time of the queue to unpause_time.
  virtual void SetPfcPaused(uint32_t port_id, int priority,
                            const omnetpp::simtime_t& unpause_time) = 0;
  // Sends out a PFC pause frame and schedules an event to send out the next
  // one. This function is only used by MMU to tell itself to send PFC frame
  // through an event via the router.
  virtual void SendPfc(uint32_t port_id, int priority) = 0;
};

// An abstract class for network router to solve the cycle dependency.
// Network router is implemented as a cSimpleModule, and the details of its APIs
// can be found in NetworkRouter class.
class NetworkRouterInterface : public omnetpp::cSimpleModule {
 public:
  virtual void RoutePacket(omnetpp::cMessage* packet) = 0;
  virtual void EnqueuePacket(omnetpp::cMessage* packet, int port_id) = 0;
  virtual void SendPacket(omnetpp::cMessage* packet, int port_id,
                          omnetpp::cDatarateChannel* transmission_channel) = 0;
  virtual void ScheduleOutput(int port_id, const omnetpp::simtime_t& delay) = 0;
  virtual void SetPfcPaused(
      PfcMessage* pfc_pause_msg,
      const omnetpp::simtime_t& start_pfc_pause_delay) = 0;
  virtual MemoryManagementUnitInterface* GetMemoryManagementUnit() const = 0;
  virtual PortInterface* GetPort(int port_id) const = 0;
  virtual bool IsPortConnected(int port_id) const = 0;
  virtual uint32_t GetMtuSize() const = 0;
  virtual int GetRouterStage() const = 0;
};

};  // namespace isekai

#endif  // ISEKAI_FABRIC_MODEL_INTERFACES_H_
