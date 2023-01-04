/*
#include "defs.h"
#include "truck.h"
#include "../droid.h"
#include "../action.h"
#include "../structure.h"
#include "../map.h"
#include "../mapgrid.h"
#include "../order.h"

namespace {

STRUCTURE *findDamagedStruct(const DROID &psDroid)
{
	STRUCTURE *psFailedTarget = nullptr;

	unsigned radius = ((psDroid.order.type == DORDER_HOLD) || (psDroid.order.type == DORDER_NONE && secondaryGetState(&psDroid, DSO_HALTTYPE) == DSS_HALT_HOLD)) ? REPAIR_RANGE : REPAIR_MAXDIST;

	unsigned bestDistanceSq = radius * radius;
	STRUCTURE *best = nullptr;

	for (BASE_OBJECT *object : gridStartIterate(psDroid.pos.x, psDroid.pos.y, radius))
	{
		unsigned distanceSq = droidSqDist(&psDroid, object);  // droidSqDist returns -1 if unreachable, (unsigned)-1 is a big number.

		STRUCTURE *structure = castStructure(object);
		if (structure == nullptr ||  // Must be a structure.
			structure == psFailedTarget ||  // Must not have just failed to reach it.
			distanceSq > bestDistanceSq ||  // Must be as close as possible.
			!visibleObject(&psDroid, structure, false) ||  // Must be able to sense it.
			!aiCheckAlliances(psDroid.player, structure->player) ||  // Must be a friendly structure.
			checkDroidsDemolishing(structure))  // Must not be trying to get rid of it.
		{
			continue;
		}

		// Check for structures to repair.
		if ((structure->status == SS_BUILT && structIsDamaged(structure)) || structure->status == SS_BEING_BUILT)
		{
			bestDistanceSq = distanceSq;
			best = structure;
		}
	}
	return best;
}


void when_idle(DROID &psDroid, const Event &event)
{
	auto where = decideWhereToRepairAndBalance(&psDroid);
	Activity::Base *activity = nullptr;
	switch (droidPeekIntention(psDroid).type)
	{
	case IntentionType::It_None:
	{
		switch (event.type)
		{
		case EventType::Ev_HealthBelowThreashold:
		{

			break;
		}	
		case EventType::Ev_DamagedStructInVicinity:
		{
			const DamagedStructInVicinity &ev = (const DamagedStructInVicinity&) event;
			activity = new Activity::MovingToStructure {ev.m_target};
			droidSetActivity(psDroid, activity);
			droidPushIntention(psDroid, Intentions::MoveToBuild {});
			break;
		}
		case EventType::Ev_OrderBuild:		// fallthrough
		case EventType::Ev_OrderRecycle:		// fallthrough
		default:
		{
			debug(LOG_ERROR, "unhandled event %i", (int) event.type);
			return;
		}
		}

		break;
	}
	case IntentionType::It_Circle:  // fallthrough
	case IntentionType::It_MoveForRepairs:  // fallthrough
	case IntentionType::It_MoveToBuild:  // fallthrough
	case IntentionType::It_MoveToRecycle:  // fallthrough
	case IntentionType::It_Patrol:  // fallthrough
	case IntentionType::It_Undefined:  // fallthrough
	default:
	{
		debug(LOG_ERROR, "unhandled current intention %i", (int) droidPeekIntention(psDroid).type);
		return;
	}
	}
}

void when_building(DROID &psDroid, const Event &event)
{
	
}

void when_moving_to_structure(DROID &psDroid, const Event &event)
{

}

void when_moving_to_place(DROID &psDroid, const Event &event)
{

}

void when_being_repaired(DROID &psDroid, const Event &event)
{

}


void when_idle_gen_events(const DROID &psDroid)
{
	if (droid_events.find(psDroid.id) == droid_events.end()) return;
	auto event_stack = droid_events.at(psDroid.id);
	STRUCTURE *found = nullptr;
	if ((found = findDamagedStruct(psDroid)))
	{
		event_stack.emplace(DamagedStructInVicinity (found));
	}
	if (droidIsDamaged(&psDroid))
	{
		event_stack.emplace(HealthBelowThreashold {});
	}
}

}


void Constructor::tick (DROID& psDroid)
{
	if (droid_events.find(psDroid.id) == droid_events.end()) return;
	auto event_stack = droid_events.at(psDroid.id);
	if (droid_orders.find(psDroid.id) != droid_orders.end()
		&& droid_orders.at(psDroid.id) != nullptr)
	{
		Order *order = droid_orders.at(psDroid.id);
		// ignore every other event
		while (!event_stack.empty()) event_stack.pop();

		switch (order->type)
		{
		case EventType::Ev_OrderBuild:
		{
			// TODO
			// get details of order and copy relevant information to Intention
			// ...
			droidPushIntention(psDroid, Intentions::MoveToBuild {});
			break;
		}
		case EventType::Ev_OrderRecycle:
		{
			break;
		}
		case EventType::Ev_OrderRTR:
		{
			break;
		}
		default:
			break;
		}

		return;
	}
	auto cur_activity = psDroid.activity;
	while (!event_stack.empty())
	{
		const Event &next_event = event_stack.top();
		cur_activity.handle(psDroid, Constructor_tag{}, next_event);
		switch (cur_activity->type())
		{
		case ActivityType::Ac_Idle:
		{
			when_idle(psDroid, next_event);
			break;
		}
		case ActivityType::Ac_Building:
		{
			when_building(psDroid, next_event);
			break;
		}
		case ActivityType::Ac_MovingToPlace:
		{
			when_moving_to_place(psDroid, next_event);
			break;
		}
		case ActivityType::Ac_MovingToStructure:
		{
			when_moving_to_structure(psDroid, next_event);
			break;
		}
		case ActivityType::Ac_BeingRepaired:
		{
			when_being_repaired(psDroid, next_event);
			break;
		}
		default:
			debug(LOG_ERROR, "unhandled event! %i", (int) next_event.type);
		}
		event_stack.pop();
	}
}

void Constructor::gen_events(const DROID &psDroid)
{

	const auto cur_activity = psDroid.activity;
	switch (cur_activity->type())
	{
	case ActivityType::Ac_Idle:
	{
		when_idle_gen_events(psDroid);
		break;
	}
	// case ActivityType::Building:
	// {
	// 	when_building(psDroid, next_event);
	// }
	// case ActivityType::MovingToPlace:
	// {
	// 	when_moving_to_place(psDroid, next_event);
	// }
	// case ActivityType::MovingToStructure:
	// {
	// 	when_moving_to_structure(psDroid, next_event);
	// }
	// case ActivityType::BeingRepaired:
	// {
	// 	when_being_repaired(psDroid, next_event);
	// }
	default:
		debug(LOG_ERROR, "unhandled activity! %i", (int) cur_activity->type());
	}
}

*/
