#include "defs.h"
// #include "../orderdef.h"
// there is 1 tick of delay between producing an event, and handling it
// all events are consumed every tick by every droid
// orders have highest priority because given by player
// it's impossible to have multiple Orders in one tick, so we don't need a stack
// all other events have same, lower priority

// somehow map Intentions/Events types into their _Data counterpart
// https://stackoverflow.com/questions/5512910/explicit-specialization-of-template-class-member-function
/*class Backstorage
{
	template<typename V>
	void store (uint32_t droid_id, V value);
	template <>
	void store<> (uint32_t droid_id, Intentions_Circle_Data v);
	template <>
	void store (uint32_t droid_id, Intentions_Patrol_Data v);
	V obtain (uint32_t droid_id);
	bool contains (uint32_t droid_id);
}
*/

void preprocess (EventStack events) {};

bool events_are_empty (EventStack events)
{
	return  events.size () == 0;
}

Events::Type get_next_event (EventStack events)
{
	Events::Type copy = events.top ();
	events.pop ();
	return copy;
};
/*
namespace Backstorage {
// avoid name conflict with global "Intention" namespace
namespace Intentions1 {
K <Intentions::Data_MoveForRepairs> move_for_repairs;
K <Intentions::Data_MoveToBuild> move_to_build;
K <Intentions::Data_AttackTarget> attack_target;
K <Intentions::Data_MoveToRecycle> move_to_recycle;
K <Intentions::Data_MoveToPlace> move_to_place;
K <Intentions::Data_MoveAndAttack> move_and_attack;
K <Intentions::Data_Patrol> patrol;
K <Intentions::Data_Circle> circle;
K <Intentions::Data_None> none;

void pop (uint32_t droidId, Intentions::Type it)
{
	switch (it)
	{
	case Intentions::Type::None:
		Intentions1::none.at (droidId). pop ();
		break;
	case Intentions::Type::AttackTarget:
		Intentions1::attack_target.at (droidId). pop ();
		break;
	case Intentions::Type::MoveAndAttack:
		Intentions1::move_and_attack.at (droidId). pop ();
		break;
	case Intentions::Type::MoveForRepairs:
		Intentions1::move_for_repairs.at (droidId).pop ();
		break;
	case Intentions::Type::MoveToPlace:
		Intentions1::move_to_place.at (droidId).pop ();
		break;
	case Intentions::Type::MoveToBuild:
		Intentions1::move_to_build.at (droidId).pop ();
		break;
	case Intentions::Type::Circle:
		Intentions1::circle.at (droidId).pop ();
		break;
	case Intentions::Type::MoveToRecycle:
		Intentions1::move_to_recycle.at (droidId).pop ();
		break;
	case Intentions::Type::Patrol:
		Intentions1::patrol.at (droidId).pop ();
		break;
	// no "default", compilation must fail when a case is unhandled
	}
}

}

namespace Events1 {
K <Events::Data_DamagedStructInVicinity> damaged_struct_in_vicinity;
}

namespace Activity1 {

}
}
*/
//
// std::unordered_map<int, std::stack<Events::Type>> droid_events;
// std::unordered_map<int, > back_storage_intentions;
