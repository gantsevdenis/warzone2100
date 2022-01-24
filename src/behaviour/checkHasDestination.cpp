#include "lib/behaviouraltree/btnode.h"


class CheckHasDestination : Bt::Node
{
    BT::NodeState tick() override
    {
        const auto dest = getContext("destination");
        return dest? BT::NodeState::SUCCESS : BT::NodeState::FAILURE ;
    }
}