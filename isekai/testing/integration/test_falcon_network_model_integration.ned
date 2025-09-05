network TestFalconNetworkIntegration
{
  parameters:
    string simulation_config;

  submodules:
    router: NetworkRouter {
      router_name = "router";
      num_port = 3;
    }
    host[2]: FalconHost;

  connections:
    host[0].in <-- {datarate=200Gbps; delay=245ns;} <-- router.port$o[0];
    host[0].out --> {datarate=200Gbps; delay=245ns;} --> router.port$i[0];
    host[1].in <-- {datarate=200Gbps; delay=245ns;} <-- router.port$o[1];
    host[1].out --> {datarate=200Gbps; delay=245ns;} --> router.port$i[1];
}
