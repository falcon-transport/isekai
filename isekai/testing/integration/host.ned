simple Host
{
  parameters:
    @networkNode;
    // The host_id is used to get the correct host config from simulation config proto text.
    string host_id;
    string host_ip;

  gates:
    input in;
    output out;
}
