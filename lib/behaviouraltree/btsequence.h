#include "btnode.h"

namespace BT
{
    struct Sequence: Node
    {
        public:
        NodeState tick() override;
    }
}