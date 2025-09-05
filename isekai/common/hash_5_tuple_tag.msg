cplusplus {{
#include "inet/common/INETDefs.h"
#include "omnetpp/cobject.h"
}}

class Hash5TupleTag extends omnetpp::cObject
{
  // Stores the 5-tuple hash computed as crc32 of string concatenation of:
  // src_ip, dst_ip, ip_proto, src_port, dst_port and flow_label.
  uint32_t hash5tuple;
}
