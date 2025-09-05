cplusplus {{
#include "omnetpp/cobject.h"
}}

class IngressPacketTag extends omnetpp::cObject
{
    int portId;
    int priority;
    int minimalGuaranteeOccupancy;
    int sharedPoolOccupancy;
    int headroomOccupancy;
}