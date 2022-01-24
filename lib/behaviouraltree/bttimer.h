#include "btnode.h"

namespace BT
{
    struct Timer: Node
    {
        public:
        NodeState tick () override;
    };
}