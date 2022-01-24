#ifndef _btnode_h
#define _btnode_h

#include <vector>
#include <string>
#include <unordered_map>
namespace BT 
{
    enum NodeState
    {
        UNDEFINED,
        RUNNING,
        SUCCESS,
        FAILURE,        
    };

    struct Node
    {
        public:
        Node(std::string name) : _name(name) {};       
        virtual NodeState tick() = 0;

        virtual void addChild(Node *child)
        {
            child->_parent = this;
            _children.push_back(child);
        }

        virtual void stop()
        {
            _state = NodeState::UNDEFINED;
        }

        virtual void clearContext(std::string key)
        {
            auto item = _context.find(key);
            if (item != _context.end())
            {
                _context.erase(item);
            }
            Node *p = _parent;
            while (p != nullptr)
            {
                p->clearContext(key);
                p = p->_parent;
            }
        }
        
        virtual void* getContext(std::string key)
        {
            auto item = _context.find(key);
            if (item != _context.end())
            {
                return item->second;
            }
            Node *p = _parent;
            while (p != nullptr)
            {
                p->getContext(key);
                if (p) return p;
                p = p->_parent;
            }
            return nullptr;
        }
        protected:
        NodeState _state;
        std::string _name;
        std::vector<Node*> _children;
        std::unordered_map<std::string, void*> _context;

        private:
        Node *_parent;
        
    };
}


#endif