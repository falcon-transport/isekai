import inet.node.ethernet.Eth100G;

network TestFalconNetwork
{
  parameters:
    string simulation_config;

  submodules:
    host[2]: FalconHost;

  connections:
    host[0].in <-- Eth100G <-- host[1].out;
    host[0].out --> Eth100G --> host[1].in;
}
