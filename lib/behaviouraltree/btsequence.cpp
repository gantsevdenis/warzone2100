#include "btsequence.h"

namespace BT
{
    NodeState Sequence::tick()
    {
        bool isRunning = false;
        for(Node* n: _children)
        {
            switch (n->tick())
            {
            case NodeState::FAILURE:
                _state = NodeState::FAILURE;
                return _state;
            case NodeState::SUCCESS:
                continue;
            case NodeState::RUNNING:
                isRunning = true;
                continue;
            case NodeState::UNDEFINED:
                _state = NodeState::UNDEFINED;
                return _state;
            }
        }
        _state = isRunning? NodeState::RUNNING : NodeState::SUCCESS;
        return _state;
    }
}