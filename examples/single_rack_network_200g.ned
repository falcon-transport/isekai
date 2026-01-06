import inet.node.ethernet.Eth200G;

network SingleRackNetwork200G
{
  parameters:
    string simulation_config;

  submodules:
    router: NetworkRouter {
      router_name = "router";
      num_port = 100;
      routing_table = "isekai/test_data/nib_snapshot/routing_64.recordio";
    }
    host_0: FalconHost {
      host_id = "host_0";
      host_ip = "fc00:0100:500:b::1";
    }
    host_1: FalconHost {
      host_id = "host_1";
      host_ip = "fc00:0100:500:c::1";
    }
    host_2: FalconHost {
      host_id = "host_2";
      host_ip = "fc00:0100:500:d::1";
    }
    host_3: FalconHost {
      host_id = "host_3";
      host_ip = "fc00:0100:500:e::1";
    }
    host_4: FalconHost {
      host_id = "host_4";
      host_ip = "fc00:0100:500:f::1";
    }
    host_5: FalconHost {
      host_id = "host_5";
      host_ip = "fc00:0100:500:10::1";
    }
    host_6: FalconHost {
      host_id = "host_6";
      host_ip = "fc00:0100:500:11::1";
    }
    host_7: FalconHost {
      host_id = "host_7";
      host_ip = "fc00:0100:500:12::1";
    }
    host_8: FalconHost {
      host_id = "host_8";
      host_ip = "fc00:0100:500:13::1";
    }
    host_9: FalconHost {
      host_id = "host_9";
      host_ip = "fc00:0100:500:14::1";
    }
    host_10: FalconHost {
      host_id = "host_10";
      host_ip = "fc00:0100:500:15::1";
    }
    host_11: FalconHost {
      host_id = "host_11";
      host_ip = "fc00:0100:500:16::1";
    }
    host_12: FalconHost {
      host_id = "host_12";
      host_ip = "fc00:0100:500:17::1";
    }
    host_13: FalconHost {
      host_id = "host_13";
      host_ip = "fc00:0100:500:18::1";
    }
    host_14: FalconHost {
      host_id = "host_14";
      host_ip = "fc00:0100:500:19::1";
    }
    host_15: FalconHost {
      host_id = "host_15";
      host_ip = "fc00:0100:500:1a::1";
    }
    host_16: FalconHost {
      host_id = "host_16";
      host_ip = "fc00:0100:500:1b::1";
    }
    host_17: FalconHost {
      host_id = "host_17";
      host_ip = "fc00:0100:500:1c::1";
    }
    host_18: FalconHost {
      host_id = "host_18";
      host_ip = "fc00:0100:500:1d::1";
    }
    host_19: FalconHost {
      host_id = "host_19";
      host_ip = "fc00:0100:500:1e::1";
    }
    host_20: FalconHost {
      host_id = "host_20";
      host_ip = "fc00:0100:500:1f::1";
    }
    host_21: FalconHost {
      host_id = "host_21";
      host_ip = "fc00:0100:500:20::1";
    }
    host_22: FalconHost {
      host_id = "host_22";
      host_ip = "fc00:0100:500:21::1";
    }
    host_23: FalconHost {
      host_id = "host_23";
      host_ip = "fc00:0100:500:22::1";
    }

  connections:
    host_0.in <-- Eth200G <-- router.port$o[10];
    host_0.out --> Eth200G --> router.port$i[10];
    host_1.in <-- Eth200G <-- router.port$o[11];
    host_1.out --> Eth200G --> router.port$i[11];
    host_2.in <-- Eth200G <-- router.port$o[12];
    host_2.out --> Eth200G --> router.port$i[12];
    host_3.in <-- Eth200G <-- router.port$o[13];
    host_3.out --> Eth200G --> router.port$i[13];
    host_4.in <-- Eth200G <-- router.port$o[14];
    host_4.out --> Eth200G --> router.port$i[14];
    host_5.in <-- Eth200G <-- router.port$o[15];
    host_5.out --> Eth200G --> router.port$i[15];
    host_6.in <-- Eth200G <-- router.port$o[16];
    host_6.out --> Eth200G --> router.port$i[16];
    host_7.in <-- Eth200G <-- router.port$o[17];
    host_7.out --> Eth200G --> router.port$i[17];
    host_8.in <-- Eth200G <-- router.port$o[18];
    host_8.out --> Eth200G --> router.port$i[18];
    host_9.in <-- Eth200G <-- router.port$o[19];
    host_9.out --> Eth200G --> router.port$i[19];
    host_10.in <-- Eth200G <-- router.port$o[20];
    host_10.out --> Eth200G --> router.port$i[20];
    host_11.in <-- Eth200G <-- router.port$o[21];
    host_11.out --> Eth200G --> router.port$i[21];
    host_12.in <-- Eth200G <-- router.port$o[22];
    host_12.out --> Eth200G --> router.port$i[22];
    host_13.in <-- Eth200G <-- router.port$o[23];
    host_13.out --> Eth200G --> router.port$i[23];
    host_14.in <-- Eth200G <-- router.port$o[24];
    host_14.out --> Eth200G --> router.port$i[24];
    host_15.in <-- Eth200G <-- router.port$o[25];
    host_15.out --> Eth200G --> router.port$i[25];
    host_16.in <-- Eth200G <-- router.port$o[26];
    host_16.out --> Eth200G --> router.port$i[26];
    host_17.in <-- Eth200G <-- router.port$o[27];
    host_17.out --> Eth200G --> router.port$i[27];
    host_18.in <-- Eth200G <-- router.port$o[28];
    host_18.out --> Eth200G --> router.port$i[28];
    host_19.in <-- Eth200G <-- router.port$o[29];
    host_19.out --> Eth200G --> router.port$i[29];
    host_20.in <-- Eth200G <-- router.port$o[30];
    host_20.out --> Eth200G --> router.port$i[30];
    host_21.in <-- Eth200G <-- router.port$o[31];
    host_21.out --> Eth200G --> router.port$i[31];
    host_22.in <-- Eth200G <-- router.port$o[32];
    host_22.out --> Eth200G --> router.port$i[32];
    host_23.in <-- Eth200G <-- router.port$o[33];
    host_23.out --> Eth200G --> router.port$i[33];
}
