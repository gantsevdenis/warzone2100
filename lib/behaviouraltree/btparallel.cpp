#include "btparallel.h"

namespace BT
{
    NodeState Parallel::tick()
    {
        uint32_t failed = 0;
        bool isRunning = false;
        for (Node *kid: _children)
        {
            switch (kid->tick())
            {
            case NodeState::FAILURE:
                failed++;
                break;
            case NodeState::RUNNING:
                isRunning = true;
                return _state;
            case NodeState::SUCCESS:
                _state = NodeState::SUCCESS;
                return _state;
            case NodeState::UNDEFINED:
                break;
            }
        }
        if (failed == _children.size())
        {
            _state = NodeState::FAILURE;
        }
        else
        {
            _state = isRunning ? NodeState::RUNNING : NodeState::SUCCESS;
        }
        return _state;
    }
}