#include "btselector.h" 
namespace BT 
{
    NodeState Selector::tick()
    {
        for (Node *kid: _children)
        {
            switch (kid->tick())
            {
            case NodeState::FAILURE:
                /* code */
                break;
            case NodeState::RUNNING:
                _state = NodeState::RUNNING;
                return _state;
            case NodeState::SUCCESS:
                _state = NodeState::SUCCESS;
                return _state;
            case NodeState::UNDEFINED:
                break;
            }
        }
    }
}