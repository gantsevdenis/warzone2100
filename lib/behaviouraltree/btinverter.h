#include "btnode.h"

namespace BT
{
    struct Inverter: Node
    {
        public:
        NodeState tick () override;
    };
}