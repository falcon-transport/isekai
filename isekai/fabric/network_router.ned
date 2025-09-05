simple NetworkRouter {
  parameters:
    @networkNode;
    string router_name;
    // The number of ports.
    int num_port = default(0);
    // The routing table file path.
    string routing_table;
    // The stage of the router (S1/S2/S3).
    int stage = default(1);

  gates:
    inout port[num_port] @loose;
}