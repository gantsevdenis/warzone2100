#ifndef __INCLUDED_BEHAVIOR_DEFH__
#define __INCLUDED_BEHAVIOR_DEFH__

#include <unordered_map>
#include <stack>
#include "../orderdef.h"
#include "../order.h"

// should be within Droid itself
// basically replaces DROID_ACTION
namespace Personnality {
	class Any {};
	class Constructor {};
	class Ground_Direct_Weapon{};
	class Ground_Indirect_Weapon{};
	class Ground_AA{};
	class Ground_Sensor{};
	class Ground_CB{};
	class Ground_VTOL_CB{};
	class Ground_VTOL_Strike{};
	class Ground_Radar_Detector{};
	class Ground_Radar_CB{};
	class Ground_Commander{};
	class Ground_MRT{};
	class Ground_Wide_Sensor{};
	class Ground_EMP{};
	class VTOL_Weapon{};
	class VTOL_Transport{};
}

// replaces DROID_ACTION
namespace Activity {
	enum class Type
	{
		Idle,
		Building,  // constructing a new, or repairing an unfinished structure
		Repairing, // repairing another droid
		MovingToStructure,
		MovingToPlace,
		MovingToDroid,
		BeingRepaired,
		BeingRearmed,
		AttackingDroid,
		AttackingStructure
	};
// TODO need something like a blackboard to store related pointers
	class Idle {};
	class Building {};
	class Repairing {};
	class MovingToStructure {};
	class MovingToPlace {};
	class MovingToDroid {};
	class BeingRepaired {};
}

namespace Events {
	enum class Type
	{
		HealthBelowThreashold,
		Repaired,
		DamagedStructInVicinity,
		EnemiesInFiringZone,
		ArrivedAtDestination,
		BuildFinished,
		CommanderDied,
		// repair station, or MRT died
		RepairerDied,
		// when we _are artillery and our sensor went kaput
		// might be Sensor/CB/VTOL-CB/VTOL-Strike/Radar/Radar-CB/WideSensor
		MySensorDied,
		// for ex, target moved on an island, and we are not a HOVER
		// or Wall was built between us and target
		// or might be triggered when havent moved in a while (?)
		NoRouteToTarget,
		NoRouteToPlace,
		// target is out of our optimum/short/long range.
		// TODO what about Target Out Of Visibility Zone?
		TargetWentTooFar,
		TargetDied,
		TargetChangedOwner,
		Timeout // maybe was going somewhere but got stuck. Become Idle, or whatever
	};
	class HealthBelowThreashold {};
	class Repaired {};
	class DamagedStructInVicinity {};
	class EnemiesInFiringZone {};
	class TargetDied {};
	class BuildFinished {};
	class CommanderDied {};
	class ArrivedAtDestination {};
	class RepairerDied {};
	class MySensorDied {};
	class NoRouteToTarget {};
	class NoRouteToPlace {};
	class TargetWentTooFar {};
	class Timeout {};

	/*class Data_DamagedStructInVicinity
	{
	public:
	Data_DamagedStructInVicinity (STRUCTURE *structure): m_structure (structure) {}
	STRUCTURE *m_structure;
	};*/

}

// This is our "stack alphabet" in the sense oft a pushdown automation
namespace Intentions {
	enum class Type
	{
		None,
		// simply clicked somewhere, or maybe ai decided to go?
		MoveToPlace,
		MoveToBuild,
		MoveToRecycle,
		MoveForRepairs,
		// Attack a particular enemy
		AttackTarget,
		// Attack everything while moving
		MoveAndAttack,
		Patrol,
		Circle
	};
	class None {};
	class MoveForRepairs {};
	class AttackTarget {};
	class MoveToRepair {};
	class MoveToBuild {};
	class MoveToRecycle {};
	class MoveToPlace {};
	class MoveAndAttack {};
	class Patrol {};
	class Circle {};


	// specific data for each intention
	/*class Data_MoveForRepairs
	{
	public:
	Data_MoveForRepairs (RtrBestResult target): m_target (target) {}
	RtrBestResult m_target;
	};
	
	class Data_MoveToBuild {};
	class Data_AttackTarget {};
	class Data_MoveToRecycle {};
	class Data_MoveToPlace {};
	class Data_MoveAndAttack {};
	class Data_Patrol {};
	class Data_Circle {};
	class Data_None {};*/
	
}

namespace Backstorage {
// I need to push/pop this stack in sync with
// intention stack
template <typename T> using K = std::unordered_map<uint32_t, std::stack<T>>;

namespace Intentions1 {

extern K <Intentions::Data_MoveForRepairs> move_for_repairs;
extern K <Intentions::Data_MoveToBuild> move_to_build;
extern K <Intentions::Data_AttackTarget> attack_target;
extern K <Intentions::Data_MoveToRecycle> move_to_recycle;
extern K <Intentions::Data_MoveToPlace> move_to_place;
extern K <Intentions::Data_MoveAndAttack> move_and_attack;
extern K <Intentions::Data_Patrol> patrol;
extern K <Intentions::Data_Circle> circle;
extern K <Intentions::Data_None> none;
extern void pop (uint32_t droidId, Intentions::Type it);
}
/*
namespace Events1 {
// we dont need a storage for Events, because they are handled fluently
extern K <Events::Data_DamagedStructInVicinity> damaged_struct_in_vicinity;
}

// we don't need a storage for Activity, because Activity replaces Action
// and it already works with droid->psActiontarget
*/

}

// those combination which aren't present are implicitely ignored
// to explicitely ignore, create the handler which does nothing (maybe log it)
namespace Constructor {
	void gen_events (const DROID &droid);
}

#define EVENT_HANDLER_INTRO(Intention_Type, Activity_Type) static void call (DROID &droid, Intentions::Intention_Type it, Activity::Activity_Type ac, EventStack events)\
	{\
		while (1)\
		{\
			if (events_are_empty (events)) break;\
			Events::Type event = get_next_event (events);\
			switch (event)\
			{

#define EVENT_HANDLER_OUTRO 	default: debug(LOG_ERROR, "unhandled event %i for droid %i", (int) event, droid.id); }\
		}\
	}

#define EVENT_HANDLER_1(Next, Intention_Type, Activity_Type, Event1) \
	EVENT_HANDLER_INTRO(Intention_Type, Activity_Type) \
	case Events::Type::Event1: return ::Next (droid, it, ac, Events::Event1 {});\
	EVENT_HANDLER_OUTRO

#define EVENT_HANDLER_2(Next, Intention_Type, Activity_Type, Event1, Event2) \
	EVENT_HANDLER_INTRO(Intention_Type, Activity_Type) \
	case Events::Type::Event1: return ::Next (droid, it, ac, Events::Event1 {});\
	case Events::Type::Event2: return ::Next (droid, it, ac, Events::Event2 {});\
	EVENT_HANDLER_OUTRO

#define EVENT_HANDLER_3(Next, Intention_Type, Activity_Type, Event1, Event2, Event3) \
	EVENT_HANDLER_INTRO(Intention_Type, Activity_Type) \
	case Events::Type::Event1: return ::Next (droid, it, ac, Events::Event1 {});\
	case Events::Type::Event2: return ::Next (droid, it, ac, Events::Event2 {});\
	case Events::Type::Event3: return ::Next (droid, it, ac, Events::Event3 {});\
	EVENT_HANDLER_OUTRO

#define EVENT_HANDLER_4(Next, Intention_Type, Activity_Type, Event1, Event2, Event3, Event4) \
	EVENT_HANDLER_INTRO(Intention_Type, Activity_Type) \
	case Events::Type::Event1: return ::Next (droid, it, ac, Events::Event1 {});\
	case Events::Type::Event2: return ::Next (droid, it, ac, Events::Event2 {});\
	case Events::Type::Event3: return ::Next (droid, it, ac, Events::Event3 {});\
	case Events::Type::Event4: return ::Next (droid, it, ac, Events::Event4 {});\
	EVENT_HANDLER_OUTRO

#define EVENT_HANDLER_5(Next, Intention_Type, Activity_Type, Event1, Event2, Event3, Event4, Event5) \
	EVENT_HANDLER_INTRO(Intention_Type, Activity_Type) \
	case Events::Type::Event1: return ::Next (droid, it, ac, Events::Event1 {});\
	case Events::Type::Event2: return ::Next (droid, it, ac, Events::Event2 {});\
	case Events::Type::Event3: return ::Next (droid, it, ac, Events::Event3 {});\
	case Events::Type::Event4: return ::Next (droid, it, ac, Events::Event4 {});\
	case Events::Type::Event5: return ::Next (droid, it, ac, Events::Event5 {});\
	EVENT_HANDLER_OUTRO

#define EVENT_HANDLER_6(Next, Intention_Type, Activity_Type, Event1, Event2, Event3, Event4, Event5, Event6) \
	EVENT_HANDLER_INTRO(Intention_Type, Activity_Type) \
	case Events::Type::Event1: return ::Next (droid, it, ac, Events::Event1 {});\
	case Events::Type::Event2: return ::Next (droid, it, ac, Events::Event2 {});\
	case Events::Type::Event3: return ::Next (droid, it, ac, Events::Event3 {});\
	case Events::Type::Event4: return ::Next (droid, it, ac, Events::Event4 {});\
	case Events::Type::Event5: return ::Next (droid, it, ac, Events::Event5 {});\
	case Events::Type::Event6: return ::Next (droid, it, ac, Events::Event6 {});\
	EVENT_HANDLER_OUTRO

	
#define DISPATCH_ACTIVITY_INTRO(Intention_Type, Next) void dispatch_activity_ ## Next (DROID &droid, Intentions::Intention_Type it, EventStack events)	\
{\
	switch (droid.activity)\
	{
	
#define DISPATCH_ACTIVITY_OUTRO 	default: debug (LOG_ERROR, "unhandled activity %i for droid %i", (int) droid.activity, droid.id); \
	}\
}

#define DISPATCH_ACTIVITY_1(Intention_Type, Next, Activity1)	\
DISPATCH_ACTIVITY_INTRO(Intention_Type, Next)\
case Activity::Type::Activity1: return Next::call (droid, Intentions::Intention_Type {}, Activity::Activity1 {}, events);\
DISPATCH_ACTIVITY_OUTRO

#define DISPATCH_ACTIVITY_2(Intention_Type, Next, Activity1, Activity2)	\
DISPATCH_ACTIVITY_INTRO(Intention_Type, Next)\
case Activity::Type::Activity1: return Next::call (droid, Intentions::Intention_Type {}, Activity::Activity1 {}, events);\
case Activity::Type::Activity2: return Next::call (droid, Intentions::Intention_Type {}, Activity::Activity2 {}, events);\
DISPATCH_ACTIVITY_OUTRO

#define DISPATCH_ACTIVITY_3(Intention_Type, Next, Activity1, Activity2, Activity3)	\
DISPATCH_ACTIVITY_INTRO(Intention_Type, Next)\
case Activity::Type::Activity1: return Next::call (droid, Intentions::Intention_Type {}, Activity::Activity1 {}, events);\
case Activity::Type::Activity2: return Next::call (droid, Intentions::Intention_Type {}, Activity::Activity2 {}, events);\
case Activity::Type::Activity3: return Next::call (droid, Intentions::Intention_Type {}, Activity::Activity3 {}, events);\
DISPATCH_ACTIVITY_OUTRO

#define DISPATCH_INTENTIONS_INTRO(Next) \
void dispatch_intentions_ ## Next (DROID &droid, EventStack events)\
{\
	switch (droidPeekIntention(droid))\
	{
#define DISPATCH_INTENTIONS_OUTRO \
	default: debug (LOG_ERROR, "unhandled intention %i for droid %i",  (int) droidPeekIntention (droid), droid.id);\
	}\
}

#define DISPATCH_INTENTIONS_6(Next, Intention1, Intention2, Intention3, Intention4, Intention5, Intention6)\
	DISPATCH_INTENTIONS_INTRO(Next) \
	case Intentions::Type::Intention1: return dispatch_activity_ ## Next (droid, Intentions::Intention1 {}, events);\
	case Intentions::Type::Intention2: return dispatch_activity_ ## Next (droid, Intentions::Intention2 {}, events);\
	case Intentions::Type::Intention3: return dispatch_activity_ ## Next (droid, Intentions::Intention3 {}, events);\
	case Intentions::Type::Intention4: return dispatch_activity_ ## Next (droid, Intentions::Intention4 {}, events);\
	case Intentions::Type::Intention5: return dispatch_activity_ ## Next (droid, Intentions::Intention5 {}, events);\
	case Intentions::Type::Intention6: return dispatch_activity_ ## Next (droid, Intentions::Intention6 {}, events);\
	DISPATCH_INTENTIONS_OUTRO

// there is 1 tick of delay between producing an event, and handling it
// all events are consumed every tick by every droid
// orders have highest priority because given by player
// it's impossible to have multiple Orders in one tick, so we don't need a stack
//extern std::unordered_map<int, Order*> droid_orders;
// all other events have same, lower priority
using EventStack = std::stack<Events::Type>;
extern std::unordered_map<int, EventStack> droid_events;

extern void preprocess (EventStack events);
extern bool events_are_empty (EventStack events);
extern Events::Type get_next_event (EventStack events);

#endif
