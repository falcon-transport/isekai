import inet.node.ethernet.Eth100G;

network TestFalconNetworkNdv6Flows
{
  parameters:
    string simulation_config;

  submodules:
    router: NetworkRouter {
      router_name = "router";
      num_port = 50;
    }
    host[2]: FalconHost;

  connections:
    host[0].in <-- Eth100G <-- router.port$o[39];
    host[0].out --> Eth100G --> router.port$i[39];
    host[1].in <-- Eth100G <-- router.port$o[42];
    host[1].out --> Eth100G --> router.port$i[42];
}
