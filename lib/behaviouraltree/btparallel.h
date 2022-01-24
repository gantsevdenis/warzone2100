#include "btnode.h"
namespace BT
{
    struct Parallel: Node
    {
        public:
        NodeState tick() override;
    }
}