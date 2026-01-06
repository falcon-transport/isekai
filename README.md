## Isekai README

**Falcon** is a hardware-offloaded network transport designed for
general-purpose Ethernet datacenter environments (with losses and without
special switch support). It supports heterogeneous application workloads across
multiple Upper Layer Protocols (ULPs) such as RDMA and gNVMe.

Falcon's design includes: delay-based congestion control with multipath load
balancing; a layered design with a simple request-response transaction interface
for multi-ULP support; hardware-based retransmissions and error-handling for
scalability; and a programmable engine for flexibility.

**Isekai** is an event-driven network simulator that implements the Falcon
protocol. It also includes models of an RDMA NIC, network switch and traffic
generator to run end-to-end network simulations. The RDMA model is infinitely
fast while the Falcon model is an accurate timing model, specifically with
queuing and scheduling points accurately modeled. Neither model supports caching
(QueuePair and connection caches).

### Overview

Isekai is composed of the following core parts:

1.  R-NIC: Models an RDMA-capable NIC implementing RDMA, Falcon and a Packet
    Builder module.
2.  Fabric: Models a shared memory network switch to connect multiple NICs
    together.
3.  TrafficGenerator: Models a host application running arbitrary workload by
    generating synthetic traffic.

#### R-NIC

R-NIC models the RDMA and Falcon protocols. The `OmnestPacketBuilder` model
converts internal packet format to OMNest INET compatible packet format.

##### RDMA

`isekai/host/rdma/`

`RDMABaseModel` provides the core logic for simulating RDMA operations. It
manages QPs, schedules operations, and provides a framework for different RDMA
implementations.

##### Falcon

`isekai/host/falcon/`

`FalconModel` models the Falcon transport protocol. It employs multiple internal
modules:

-   `AckCoalescingEngine` is responsible for generating coalesced ACKs and
    NACKs.
-   `AckNackScheduler` handles arbitration of ACK/NACK packets with initial
    transmissions and retransmissions/resyncs in an unified manner.
-   `BufferReorderEngine` handles RX buffer reordering that accepts packets in
    any RSN/SSN order on per-connection basis.
-   `ConnectionScheduler` arbitrates TX packets across multiple connections
-   `ConnectionStateManager` manages the state corresponding to FALCON
    connections.
-   `PacketReliabilityManager` implements sliding window, gating criteria, loss
    recovery through retransmissions and handling/generating ACK/NACKs.
-   `RateUpdateEngine` (RUE) receives information about network conditions with
    `RueEvent`, and runs a congestion control algorithm to adjust the
    transmission window (rate), and feeds the updated parameters back to the
    Falcon datapath with `RueResponse`.
-   `ResourceManager` manages Falcon block resource pools across connections.
-   `RetransmissionScheduler` is responsible scheduling retransmitted packets.

Note, the model does not currently support the ReSync protocol, and the
solicitation feature is experimental.

#### Traffic Generator

`isekai/host/traffic/`

`TrafficGenerator` is responsible for simulating RDMA workloads. It acts as an
application that drives RDMA operations, generating synthetic traffic patterns
to test and evaluate network performance.

### Quick Start

#### What you need to build Isekai

-   **[Bazel](https://bazel.build/)**
-   **[Python3-dev](https://stackoverflow.com/questions/6230444/how-to-install-python-developer-package)**
-   **[OMNeT++](https://github.com/omnetpp/omnetpp)** (requires commercial
    license)
-   **[Pandas](https://pandas.pydata.org/docs/getting_started/install.html)**
-   **[Texinfo](https://www.gnu.org/software/texinfo/)**

#### External Dependencies (automatically download and compile)

-   ABSL
-   INET
-   CRC32
-   Folly
-   Protobuf
-   glog
-   Googletest
-   Riegeli

#### Supported Platforms

Currently, Isekai is tested for and supports Linux environment.

#### Codebase Structures

-   `bazel/`: Bazel configs for external dependencies and build rules.
-   `examples/`: a few example simulation configs.
-   `isekai/`: Isekai source codes.
    -   `common/`: utility libraries and protobufs commonly used across Isekai
        components.
    -   `fabric/`: fabric implementation of Isekai including router and port
        selection.
    -   `host/`: host implementation of Isekai including Falcon, RDMA, and
        traffic generator.
    -   `omnetpp/`: wrapper for OMNeT++ to be linked with Isekai.
    -   `scripts/`: scripts for parsing and post-processing the simulation
        results.
    -   `test_data/`: metadata required for example network including topology
        and routing tables.

#### Building Isekai

Isekai can be built using [Bazel](https://bazel.build/). Run from the root
directory of the repository:

```
bazel build -c opt "..."
```

It takes 5-10 mins to build for the first time.

#### Running Unit Tests

To verify the functionalities of Isekai, run the following command under the
root directory of the Isekai codebase to execute all the unit tests.

```
bazel test -c opt "..."
```

#### Running Isekai

Isekai requires three inputs to simulate a scenario:

-   `.ini` contains high-level configuration including simulation time, which
    network to use for simulation, and the path to the simulation config file.
    Note that Isekai current supports simulation time up to 2^24 * 131.072 ns ~
    2.199 seconds.
-   `.ned` defines the network including hosts, routers, routing tables, and
    topology.
-   `.pb.txt` configures the simulation including host configs, router configs,
    and traffic patterns to generate. Refer to `isekai/common/config.proto` for
    the full list of configurable parameters.

We provide three experiment scenarios under `/examples`:

-   `single_rack_uniform_random_200g`: 24-to-24 uniform random traffic pattern
    where there are 24 hosts under the same rack. Each host choose a destination
    host randomly and issue 128 KB WRITE operation with a rate of 120 Gbps.
-   `incast_simulation`: 10-to-1 incast traffic pattern where 10 hosts from 10
    different racks generate traffic to one victim host located in another rack
    with 128 KB WRITE operation at a rate of 10 Gbps per sender.
-   `single_rack_incast_(5|50|500)_to_1_200g`: 5/50/500-to-1 single incast
    traffic pattern where 5/50/500 QPs from 5 hosts generate traffic to one
    victim host where all sender hosts and the victim host are connected to the
    same rack. Each QP issues 1 MB WRITE operation with a rate of 38/3.8/0.38
    Gbps with open loop uniform time distribution so that victim host can
    receive the traffic 95% of the linerate (200 Gbps) on average with bursts.
-  `single_rack_p2p_rack_tlp_800g`: point-to-point traffic pattern where both
    initiator and target are under the same rack. Initiator creates a single QP
    and generates 128KB WRITE operations at 800 Gbps rate with a Poisson arrival
    process. RTT is configured to 20us. RACK and TLP are enabled for loss
    recovery and packets are randomly dropped at 2% probability.

We provide a shell script `run_simulation.sh` to help execute simulations. To
run `single_rack_uniform_random_200g`:

```
sh run_simulation.sh -f examples/single_rack_uniform_random_200g.ini -n examples/
```

To run `incast_simulation`: `sh run_simulation.sh -f
examples/incast_simulation.ini -n examples/`

The complete usage of the script is as below:

Args                                | Required
----------------------------------- | --------
-f \<simulation config ini file\>   | Yes
-n \<NED file search paths\>        | No
-c \<config section\>               | No
-o \<simulation result output dir\> | No

#### Parsing Simulation Results

Once the simulation finishes, it will show where the simulation results are
stored. For example:

```
###Simulation Succeeds###
Config file: /falcon-network-simulator/examples/single_rack_uniform_random_200g.ini
Simulation results in: /falcon-network-simulator/output-single_rack_uniform_random_200g
##########
```

Under the folder storing the simulation results, there are several types of
files:

-   .sca file stores the scalar value of the collected statistics. See
    [here](https://doc.omnetpp.org/omnetpp/manual/#sec:ana-sim:scalar-result-files)
    for more information.
-   .vec file stores the time series data. See
    [here](https://doc.omnetpp.org/omnetpp/manual/#sec:ana-sim:output-vector-files)
    for more information.
-   \<host_id\>_falcon_latency_histograms stores the breakdown latency inside
    the Falcon protocol.
-   \<host_id\>_rdma_latency_histograms stores the breakdown latency inside the
    RDMA.

We provide two example scripts for parsing goodput from `.sca` and
min/mean/median/p99/p999/max vector metrics from `.vec`.

To parse scalar metrics from `.sca`:

```
python3 isekai/scripts/process_stats_sca.py <path/to/.sca> <metric_name> [--group_by=<group_by>] [--reducer=<reducer>] [--is_rate]
```

To parse min/mean/median/p99/p999/max vector metrics from `.vec`:

```
python3 isekai/scripts/process_stats_vec.py <path/to/.vec> <metric_name>
```
