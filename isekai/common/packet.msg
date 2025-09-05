cplusplus {{
#include "isekai/common/packet.h"
#include "inet/common/packet/chunk/FieldsChunk.h"
}}

struct isekai::Packet {
  @existingClass;
}


class RocePacket extends inet::FieldsChunk {
  isekai::Packet roceContent;
}

class inet::FieldsChunk {
  @existingClass;
}

class FalconPacket extends inet::FieldsChunk {
  isekai::Packet falconContent;
}
