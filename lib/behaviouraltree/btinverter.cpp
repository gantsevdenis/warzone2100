#include "btinverter.h"
#include "lib/framework/debug.h"

namespace BT
{
    NodeState Inverter::tick()
    {
        if (_children.size() == 0)
        {
            debug(LOG_ERROR, "inverter node has no children");
            return NodeState::UNDEFINED;
        }
        switch (_children[0]->tick())
        {
        case NodeState::FAILURE:
            return NodeState::SUCCESS;
        case NodeState::SUCCESS:
            return NodeState::FAILURE;
        case NodeState::RUNNING:
            return NodeState::RUNNING;
        case NodeState::UNDEFINED:
            return NodeState::UNDEFINED;
        }
    }
}