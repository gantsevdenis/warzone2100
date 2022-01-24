#include "btnode.h"
#include <string>
namespace BT
{
    /** "OR" Node.
     * 
    */
    struct Selector : Node
    {
        public:
        Selector(std::string &name) : Node(name) {};
        NodeState tick() override;
    };
}